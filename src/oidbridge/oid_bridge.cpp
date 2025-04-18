/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2025 OpenImageDebugger contributors
 * (https://github.com/OpenImageDebugger/OpenImageDebugger)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "oid_bridge.h"

#include <cstdint>

#include <bit>
#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "debuggerinterface/preprocessor_directives.h"
#include "debuggerinterface/python_native_interface.h"
#include "ipc/message_exchange.h"
#include "system/process/process.h"

#include <QDataStream>
#include <QTcpServer>
#include <QTcpSocket>


using namespace oid;

struct UiMessage
{
    virtual ~UiMessage() = default;
};

struct GetObservedSymbolsResponseMessage final : UiMessage
{
    std::deque<std::string> observed_symbols{};
};

struct PlotBufferRequestMessage final : UiMessage
{
    std::string buffer_name{};
};

class PyGILRAII
{
  public:
    PyGILRAII()
    {
        _py_gil_state = PyGILState_Ensure();
    }

    PyGILRAII(const PyGILRAII&)  = delete;
    PyGILRAII(const PyGILRAII&&) = delete;

    PyGILRAII& operator=(const PyGILRAII&)  = delete;
    PyGILRAII& operator=(const PyGILRAII&&) = delete;

    ~PyGILRAII()
    {
        PyGILState_Release(_py_gil_state);
    }

  private:
    PyGILState_STATE _py_gil_state{};
};

class OidBridge
{
  public:
    explicit OidBridge(int (*plot_callback)(const char*))
        : plot_callback_{plot_callback}
    {
    }

    bool start()
    {
        // Initialize server
        if (!server_.listen(QHostAddress::Any)) {
            // TODO escalate error
            std::cerr << "[OpenImageDebugger] Could not start TCP server"
                      << std::endl;
            return false;
        }

        const auto windowBinaryPath = this->oid_path_ + "/oidwindow";
        const auto portStdString    = std::to_string(server_.serverPort());

        std::vector<std::string> command{
            windowBinaryPath, "-style", "fusion", "-p", portStdString};

        ui_proc_.start(command);

        ui_proc_.waitForStart();

        wait_for_client();

        return client_ != nullptr;
    }

    void set_path(const std::string_view& oid_path)
    {
        oid_path_ = oid_path;
    }

    [[nodiscard]] bool is_window_ready() const
    {
        return client_ != nullptr && ui_proc_.isRunning();
    }

    std::deque<std::string> get_observed_symbols()
    {
        assert(client_ != nullptr);

        auto message_composer = MessageComposer{};
        message_composer.push(MessageType::GetObservedSymbols).send(client_);

        if (const auto response =
                fetch_message(MessageType::GetObservedSymbolsResponse);
            response != nullptr) {
            return dynamic_cast<GetObservedSymbolsResponseMessage*>(
                       response.get())
                ->observed_symbols;
        }

        return {};
    }


    void
    set_available_symbols(const std::deque<std::string>& available_vars) const
    {
        assert(client_ != nullptr);

        auto message_composer = MessageComposer{};
        message_composer.push(MessageType::SetAvailableSymbols)
            .push(available_vars)
            .send(client_);
    }

    void run_event_loop()
    {
        try_read_incoming_messages(static_cast<int>(1000.0 / 5.0));

        auto plot_request_message = std::make_unique<UiMessage>();
        while ((plot_request_message = try_get_stored_message(
                    MessageType::PlotBufferRequest)) != nullptr) {
            const PlotBufferRequestMessage* msg =
                dynamic_cast<PlotBufferRequestMessage*>(
                    plot_request_message.get());
            plot_callback_(msg->buffer_name.c_str());
        }
    }

    void plot_buffer(const std::string& variable_name_str,
                     const std::string& display_name_str,
                     const std::string& pixel_layout_str,
                     const bool transpose_buffer,
                     const int buff_width,
                     const int buff_height,
                     const int buff_channels,
                     const int buff_stride,
                     const BufferType buff_type,
                     const uint8_t* buff_ptr,
                     const size_t buff_length) const
    {
        auto message_composer = MessageComposer{};
        message_composer.push(MessageType::PlotBufferContents)
            .push(variable_name_str)
            .push(display_name_str)
            .push(pixel_layout_str)
            .push(transpose_buffer)
            .push(buff_width)
            .push(buff_height)
            .push(buff_channels)
            .push(buff_stride)
            .push(buff_type)
            .push(buff_ptr, buff_length)
            .send(client_);
    }

    ~OidBridge()
    {
        ui_proc_.kill();
    }

  private:
    Process ui_proc_{};
    QTcpServer server_{};
    QTcpSocket* client_{nullptr};
    std::string oid_path_{};

    int (*plot_callback_)(const char*){};

    std::map<MessageType, std::unique_ptr<UiMessage>> received_messages_{};

    std::unique_ptr<UiMessage>
    try_get_stored_message(const MessageType& msg_type)
    {
        if (const auto find_msg_handler = received_messages_.find(msg_type);
            find_msg_handler != received_messages_.end()) {
            auto result = std::move(find_msg_handler->second);
            received_messages_.erase(find_msg_handler);
            return result;
        }

        return nullptr;
    }


    void try_read_incoming_messages(const int msecs = 3000)
    {
        assert(client_ != nullptr);

        do {
            client_->waitForReadyRead(msecs);

            if (client_->bytesAvailable() == 0) {
                break;
            }

            auto header = MessageType{};
            client_->read(std::bit_cast<char*>(&header),
                          static_cast<qint64>(sizeof(header)));

            switch (header) {
            case MessageType::PlotBufferRequest:
                received_messages_[header] = decode_plot_buffer_request();
                break;
            case MessageType::GetObservedSymbolsResponse:
                received_messages_[header] =
                    decode_get_observed_symbols_response();
                break;
            default:
                std::cerr
                    << "[OpenImageDebugger] Received message with incorrect "
                       "header"
                    << std::endl;
                break;
            }
        } while (client_->bytesAvailable() > 0);
    }


    [[nodiscard]] std::unique_ptr<UiMessage> decode_plot_buffer_request() const
    {
        assert(client_ != nullptr);

        auto response        = std::make_unique<PlotBufferRequestMessage>();
        auto message_decoder = MessageDecoder{client_};
        message_decoder.read(response->buffer_name);
        return response;
    }

    [[nodiscard]] std::unique_ptr<UiMessage>
    decode_get_observed_symbols_response() const
    {
        assert(client_ != nullptr);

        auto response = std::make_unique<GetObservedSymbolsResponseMessage>();

        auto message_decoder = MessageDecoder{client_};
        message_decoder.read<std::deque<std::string>, std::string>(
            response->observed_symbols);

        return response;
    }

    std::unique_ptr<UiMessage> fetch_message(const MessageType& msg_type)
    {
        // Return message if it was already received before
        if (auto result = try_get_stored_message(msg_type); result != nullptr) {
            return result;
        }

        // Try to fetch message
        try_read_incoming_messages();

        return try_get_stored_message(msg_type);
    }


    void wait_for_client()
    {
        if (client_ == nullptr) {
            if (!server_.waitForNewConnection(10000)) {
                std::cerr << "[OpenImageDebugger] No clients connected to "
                             "OpenImageDebugger server"
                          << std::endl;
            }
            client_ = server_.nextPendingConnection();
        }
    }
};


AppHandler oid_initialize(int (*plot_callback)(const char*),
                          PyObject* optional_parameters)
{
    const auto py_gil_raii = PyGILRAII{};

    if (optional_parameters != nullptr && !PyDict_Check(optional_parameters)) {
        RAISE_PY_EXCEPTION(
            PyExc_TypeError,
            "Invalid second parameter given to oid_initialize (was expecting"
            " a dict).");
        return nullptr;
    }

    /*
     * Get optional fields
     */
    const auto py_oid_path =
        PyDict_GetItemString(optional_parameters, "oid_path");

    auto app = std::make_unique<OidBridge>(plot_callback);

    if (py_oid_path) {
        auto oid_path_str = std::string{};
        copy_py_string(oid_path_str, py_oid_path);
        app->set_path(oid_path_str);
    }

    return app.release();
}


void oid_cleanup(const AppHandler handler)
{
    const auto py_gil_raii = PyGILRAII{};

    auto app = std::unique_ptr<OidBridge>{static_cast<OidBridge*>(handler)};

    if (!app) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_terminate received null application handler");
        return;
    }

    app.reset();
}


void oid_exec(const AppHandler handler)
{
    const auto py_gil_raii = PyGILRAII{};

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_exec received null application handler");
        return;
    }

    app->start();
}


int oid_is_window_ready(const AppHandler handler)
{
    const auto py_gil_raii = PyGILRAII{};

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_exec received null application handler");
        return 0;
    }

    return app->is_window_ready();
}


PyObject* oid_get_observed_buffers(AppHandler handler)
{
    const auto py_gil_raii = PyGILRAII{};

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_Exception,
                           "oid_get_observed_buffers received null "
                           "application handler");
        return nullptr;
    }

    const auto observed_symbols = app->get_observed_symbols();
    const auto py_observed_symbols =
        PyList_New(static_cast<Py_ssize_t>(observed_symbols.size()));

    const auto observed_symbols_sentinel =
        static_cast<int>(observed_symbols.size());
    for (int i = 0; i < observed_symbols_sentinel; ++i) {
        const auto& symbol_name   = observed_symbols[i];
        const auto py_symbol_name = PyBytes_FromString(symbol_name.c_str());

        if (py_symbol_name == nullptr) {
            Py_DECREF(py_observed_symbols);
            return nullptr;
        }

        PyList_SetItem(py_observed_symbols, i, py_symbol_name);
    }

    return py_observed_symbols;
}


void oid_set_available_symbols(const AppHandler handler,
                               PyObject* available_vars)
{
    const auto py_gil_raii = PyGILRAII{};

    assert(PyList_Check(available_vars));

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_set_available_symbols received null "
                           "application handler");
        return;
    }

    auto available_vars_stl = std::deque<std::string>{};
    for (Py_ssize_t pos = 0; pos < PyList_Size(available_vars); ++pos) {
        auto var_name_str   = std::string{};
        const auto listItem = PyList_GetItem(available_vars, pos);
        copy_py_string(var_name_str, listItem);
        available_vars_stl.push_back(var_name_str);
    }

    app->set_available_symbols(available_vars_stl);
}


void oid_run_event_loop(const AppHandler handler)
{
    const auto py_gil_raii = PyGILRAII{};

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_run_event_loop received null application "
                           "handler");
        return;
    }

    app->run_event_loop();
}


void oid_plot_buffer(AppHandler handler, PyObject* buffer_metadata)
{
    const auto py_gil_raii = PyGILRAII{};

    const auto app = static_cast<OidBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "oid_plot_buffer received null application handler");
        return;
    }

    if (!PyDict_Check(buffer_metadata)) {
        RAISE_PY_EXCEPTION(PyExc_TypeError,
                           "Invalid object given to plot_buffer (was expecting"
                           " a dict).");
        return;
    }

    /*
     * Get required fields
     */
    const auto py_variable_name =
        PyDict_GetItemString(buffer_metadata, "variable_name");
    const auto py_display_name =
        PyDict_GetItemString(buffer_metadata, "display_name");
    const auto py_pointer  = PyDict_GetItemString(buffer_metadata, "pointer");
    const auto py_width    = PyDict_GetItemString(buffer_metadata, "width");
    const auto py_height   = PyDict_GetItemString(buffer_metadata, "height");
    const auto py_channels = PyDict_GetItemString(buffer_metadata, "channels");
    const auto py_type     = PyDict_GetItemString(buffer_metadata, "type");
    const auto py_row_stride =
        PyDict_GetItemString(buffer_metadata, "row_stride");
    const auto py_pixel_layout =
        PyDict_GetItemString(buffer_metadata, "pixel_layout");

    /*
     * Get optional fields
     */
    const auto py_transpose_buffer =
        PyDict_GetItemString(buffer_metadata, "transpose_buffer");
    auto transpose_buffer = false;
    if (py_transpose_buffer != nullptr) {
        CHECK_FIELD_TYPE(transpose_buffer, PyBool_Check, "transpose_buffer");
        transpose_buffer = PyObject_IsTrue(py_transpose_buffer);
    }

    /*
     * Check if expected fields were provided
     */
    CHECK_FIELD_PROVIDED(variable_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(display_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(pointer, "plot_buffer");
    CHECK_FIELD_PROVIDED(width, "plot_buffer");
    CHECK_FIELD_PROVIDED(height, "plot_buffer");
    CHECK_FIELD_PROVIDED(channels, "plot_buffer");
    CHECK_FIELD_PROVIDED(type, "plot_buffer");
    CHECK_FIELD_PROVIDED(row_stride, "plot_buffer");
    CHECK_FIELD_PROVIDED(pixel_layout, "plot_buffer");

    /*
     * Check if expected fields have the correct types
     */
    CHECK_FIELD_TYPE(variable_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(display_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(width, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(height, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(channels, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(type, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(row_stride, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(pixel_layout, check_py_string_type, "plot_buffer");

    // Retrieve pointer to buffer
    uint8_t* buff_ptr{nullptr};
    auto buff_size = std::size_t{0};
    if (PyMemoryView_Check(py_pointer) != 0) {
        get_c_ptr_from_py_buffer(py_pointer, buff_ptr, buff_size);
    } else {
        RAISE_PY_EXCEPTION(PyExc_TypeError,
                           "Could not retrieve C pointer to provided buffer");
        return;
    }

    /*
     * Send buffer contents
     */
    auto variable_name_str = std::string{};
    auto display_name_str  = std::string{};
    auto pixel_layout_str  = std::string{};

    copy_py_string(variable_name_str, py_variable_name);
    copy_py_string(display_name_str, py_display_name);
    copy_py_string(pixel_layout_str, py_pixel_layout);

    const auto buff_width    = static_cast<int>(get_py_int(py_width));
    const auto buff_height   = static_cast<int>(get_py_int(py_height));
    const auto buff_channels = static_cast<int>(get_py_int(py_channels));
    const auto buff_stride   = static_cast<int>(get_py_int(py_row_stride));

    const auto buff_type = static_cast<BufferType>(get_py_int(py_type));

    const auto buff_size_expected = std::size_t{
        static_cast<size_t>(buff_stride * buff_height * buff_channels) *
        type_size(buff_type)};

    if (buff_ptr == nullptr) {
        RAISE_PY_EXCEPTION(
            PyExc_TypeError,
            "oid_plot_buffer received nullptr as buffer pointer");
        return;
    }

    if (buff_size < buff_size_expected) {
        auto ss = std::stringstream{};
        ss << "oid_plot_buffer received shorter buffer then expected";
        ss << ". Variable name " << variable_name_str;
        ss << ". Expected " << buff_size_expected << "bytes";
        ss << ". Received " << buff_size << "bytes";
        RAISE_PY_EXCEPTION(PyExc_TypeError, ss.str().c_str());
        return;
    }

    app->plot_buffer(variable_name_str,
                     display_name_str,
                     pixel_layout_str,
                     transpose_buffer,
                     buff_width,
                     buff_height,
                     buff_channels,
                     buff_stride,
                     buff_type,
                     buff_ptr,
                     buff_size);
}
