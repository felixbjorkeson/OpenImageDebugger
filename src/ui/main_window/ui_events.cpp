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

#include "main_window.h"

#include <memory>
#include <ranges>

#include <QFileDialog>
#include <QtMath> // for portable definition of M_PI

#include "io/buffer_exporter.h"
#include "ui_main_window.h"
#include "visualization/components/buffer_values.h"
#include "visualization/components/camera.h"
#include "visualization/events.h"
#include "visualization/game_object.h"

namespace oid
{

void MainWindow::resize_callback(const int w, const int h) const
{
    for (const auto& stage : stages_ | std::views::values) {
        stage->resize_callback(w, h);
    }

    go_to_widget_->move(ui_->bufferPreview->width() - go_to_widget_->width(),
                        ui_->bufferPreview->height() - go_to_widget_->height());
}


void MainWindow::scroll_callback(const float delta)
{
    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            stage->scroll_callback(delta);
        }
    } else if (currently_selected_stage_ != nullptr) {
        currently_selected_stage_->scroll_callback(delta);
    }

    update_status_bar();

#if defined(Q_OS_DARWIN)
    ui_->bufferPreview->update();
#endif
    request_render_update_ = true;
}


void MainWindow::mouse_drag_event(const int mouse_x, const int mouse_y)
{
    const auto virtual_motion = QPoint{mouse_x, mouse_y};

    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            stage->mouse_drag_event(virtual_motion.x(), virtual_motion.y());
        }
    } else if (currently_selected_stage_ != nullptr) {
        currently_selected_stage_->mouse_drag_event(virtual_motion.x(),
                                                    virtual_motion.y());
    }

    request_render_update_ = true;
}


void MainWindow::mouse_move_event(int, int) const
{
    update_status_bar();
}


void MainWindow::resizeEvent(QResizeEvent*)
{
    persist_settings_deferred();
}


void MainWindow::moveEvent(QMoveEvent*)
{
    persist_settings_deferred();
}


void MainWindow::closeEvent(QCloseEvent*)
{
    is_window_ready_ = false;
    persist_settings_deferred();
}


void MainWindow::propagate_key_press_event(
    const QKeyEvent* key_event,
    EventProcessCode& event_intercepted) const
{
    for (const auto& stage : stages_ | std::views::values) {
        if (stage->key_press_event(key_event->key()) ==
            EventProcessCode::INTERCEPTED) {
            event_intercepted = EventProcessCode::INTERCEPTED;
        }
    }
}


bool MainWindow::eventFilter(QObject* target, QEvent* event)
{
    KeyboardState::update_keyboard_state(event);

    if (event->type() == QEvent::KeyPress) {
        const auto* key_event = dynamic_cast<QKeyEvent*>(event);

        auto event_intercepted = EventProcessCode::IGNORED;

        if (link_views_enabled_) {
            propagate_key_press_event(key_event, event_intercepted);
        } else if (currently_selected_stage_ != nullptr) {
            event_intercepted =
                currently_selected_stage_->key_press_event(key_event->key());
        }

        if (event_intercepted == EventProcessCode::INTERCEPTED) {
            request_render_update_ = true;
            update_status_bar();

            event->accept();
            return true;
        }

        return QObject::eventFilter(target, event);
    }

    return false;
}


void MainWindow::recenter_buffer()
{
    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            const auto cam_obj = stage->get_game_object("camera");
            const auto cam = cam_obj->get_component<Camera>("camera_component");
            cam->recenter_camera();
        }
    } else {
        if (currently_selected_stage_ != nullptr) {
            const auto cam_obj =
                currently_selected_stage_->get_game_object("camera");
            const auto cam = cam_obj->get_component<Camera>("camera_component");
            cam->recenter_camera();
        }
    }

    request_render_update_ = true;
}


void MainWindow::link_views_toggle()
{
    link_views_enabled_ = !link_views_enabled_;
}

void MainWindow::decrease_float_precision()
{
    const auto shift_precision_left_impl = [](Stage* stage) {
        const auto buffer_obj = stage->get_game_object("buffer");
        const auto buffer_comp =
            buffer_obj->get_component<BufferValues>("text_component");

        buffer_comp->decrease_float_precision();
    };

    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            shift_precision_left_impl(stage.get());
        }
    } else {
        if (currently_selected_stage_ != nullptr) {
            shift_precision_left_impl(currently_selected_stage_);
        }
    }

    request_render_update_ = true;
}

void MainWindow::increase_float_precision()
{
    const auto shift_precision_right_impl = [](Stage* stage) {
        const auto buffer_obj = stage->get_game_object("buffer");
        const auto buffer_comp =
            buffer_obj->get_component<BufferValues>("text_component");

        buffer_comp->increase_float_precision();
    };

    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            shift_precision_right_impl(stage.get());
        }
    } else {
        if (currently_selected_stage_ != nullptr) {
            shift_precision_right_impl(currently_selected_stage_);
        }
    }

    request_render_update_ = true;
}

void MainWindow::update_shift_precision() const
{
    if (currently_selected_stage_ != nullptr) {
        const auto buffer_obj =
            currently_selected_stage_->get_game_object("buffer");
        const auto buffer =
            buffer_obj->get_component<Buffer>("buffer_component");

        if (BufferType::Float32 == buffer->type ||
            BufferType::Float64 == buffer->type) {
            ui_->decrease_float_precision->setEnabled(true);
            ui_->increase_float_precision->setEnabled(true);
        } else {
            ui_->decrease_float_precision->setEnabled(false);
            ui_->increase_float_precision->setEnabled(false);
        }
    } else {
        ui_->decrease_float_precision->setEnabled(false);
        ui_->increase_float_precision->setEnabled(false);
    }
}

void MainWindow::rotate_90_cw()
{
    const auto request_90_cw_rotation = [](Stage* stage) {
        const auto buffer_obj = stage->get_game_object("buffer");
        const auto buffer_comp =
            buffer_obj->get_component<Buffer>("buffer_component");

        buffer_comp->rotate(90.0f * static_cast<float>(M_PI) / 180.0f);
    };

    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            request_90_cw_rotation(stage.get());
        }
    } else {
        if (currently_selected_stage_ != nullptr) {
            request_90_cw_rotation(currently_selected_stage_);
        }
    }

    request_render_update_ = true;
}


void MainWindow::rotate_90_ccw()
{
    const auto request_90_ccw_rotation = [](Stage* stage) {
        const auto buffer_obj = stage->get_game_object("buffer");
        const auto buffer_comp =
            buffer_obj->get_component<Buffer>("buffer_component");

        buffer_comp->rotate(-90.0f * static_cast<float>(M_PI) / 180.0f);
    };

    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            request_90_ccw_rotation(stage.get());
        }
    } else {
        if (currently_selected_stage_ != nullptr) {
            request_90_ccw_rotation(currently_selected_stage_);
        }
    }

    request_render_update_ = true;
}


void MainWindow::buffer_selected(QListWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }

    const auto stage =
        stages_.find(item->data(Qt::UserRole).toString().toStdString());
    if (stage != stages_.end()) {
        set_currently_selected_stage(stage->second.get());
        reset_ac_min_labels();
        reset_ac_max_labels();
        update_shift_precision();
        update_status_bar();
    }
}


void MainWindow::remove_selected_buffer()
{
    if (ui_->imageList->count() > 0 && currently_selected_stage_ != nullptr) {
        auto removed_item = std::unique_ptr<QListWidgetItem>{
            ui_->imageList->takeItem(ui_->imageList->currentRow())};
        const auto buffer_name =
            removed_item->data(Qt::UserRole).toString().toStdString();
        stages_.erase(buffer_name);
        held_buffers_.erase(buffer_name);
        removed_item.reset();

        removed_buffer_names_.insert(buffer_name);

        if (stages_.empty()) {
            set_currently_selected_stage(nullptr);
            update_shift_precision();
        }

        persist_settings_deferred();
    }
}


void MainWindow::symbol_selected()
{
    if (ui_->symbolList->text().isEmpty()) {
        return;
    }

    const auto symbol_name_qba = ui_->symbolList->text().toLocal8Bit();
    const auto symbol_name     = symbol_name_qba.constData();
    request_plot_buffer(symbol_name);
    // Clear symbol input
    ui_->symbolList->setText("");
}


void MainWindow::symbol_completed(const QString& str)
{
    if (str.isEmpty()) {
        return;
    }

    const auto symbol_name_qba = str.toLocal8Bit();
    request_plot_buffer(symbol_name_qba.constData());
    // Clear symbol input
    ui_->symbolList->setText("");
    ui_->symbolList->clearFocus();
}


void MainWindow::export_buffer()
{
    const auto sender_action(dynamic_cast<QAction*>(sender()));

    const auto stage =
        stages_.find(sender_action->data().toString().toStdString())->second;

    const auto buffer_obj = stage->get_game_object("buffer");
    const auto component =
        buffer_obj->get_component<Buffer>("buffer_component");

    auto file_dialog = QFileDialog{this};
    file_dialog.setAcceptMode(QFileDialog::AcceptSave);
    file_dialog.setFileMode(QFileDialog::AnyFile);

    auto output_extensions = QHash<QString, BufferExporter::OutputType>{};
    output_extensions[tr("Image File (*.png)")] =
        BufferExporter::OutputType::Bitmap;
    output_extensions[tr("Octave Raw Matrix (*.oct)")] =
        BufferExporter::OutputType::OctaveMatrix;

    // Generate the save suffix string
    auto it = QHashIterator{output_extensions};

    auto save_message = QString{};

    while (it.hasNext()) {
        it.next();
        save_message += it.key();
        if (it.hasNext()) {
            save_message += ";;";
        }
    }

    file_dialog.setNameFilter(save_message);
    file_dialog.selectNameFilter(default_export_suffix_);

    if (file_dialog.exec() == QDialog::Accepted) {
        const auto file_name = file_dialog.selectedFiles()[0].toStdString();
        const auto selected_filter = file_dialog.selectedNameFilter();

        // Export buffer
        BufferExporter::export_buffer(
            component, file_name, output_extensions[selected_filter]);

        // Update default export suffix to the previously used suffix
        default_export_suffix_ = selected_filter;

        // Persist settings
        persist_settings_deferred();
    }
}


void MainWindow::show_context_menu(const QPoint& pos)
{
    if (ui_->imageList->itemAt(pos) != nullptr) {
        // Handle global position
        const auto globalPos = ui_->imageList->mapToGlobal(pos);

        // Create menu and insert context actions
        auto menu = QMenu{this};

        const auto exportAction =
            menu.addAction("Export buffer", this, SLOT(export_buffer()));

        // Add parameter to action: buffer name
        exportAction->setData(ui_->imageList->itemAt(pos)->data(Qt::UserRole));

        // Show context menu at handling position
        menu.exec(globalPos);
    }
}


void MainWindow::toggle_go_to_dialog() const
{
    if (!go_to_widget_->isVisible()) {
        auto default_goal = vec4{0.0f, 0.0f, 0.0f, 0.0f};

        if (currently_selected_stage_ != nullptr) {
            const auto cam_obj =
                currently_selected_stage_->get_game_object("camera");
            const auto cam = cam_obj->get_component<Camera>("camera_component");

            default_goal = cam->get_position();
        }

        go_to_widget_->set_defaults(default_goal.x(), default_goal.y());
    }

    go_to_widget_->toggle_visible();
}


void MainWindow::go_to_pixel(const float x, const float y)
{
    if (link_views_enabled_) {
        for (const auto& stage : stages_ | std::views::values) {
            stage->go_to_pixel(x, y);
        }
    } else if (currently_selected_stage_ != nullptr) {
        currently_selected_stage_->go_to_pixel(x, y);
    }

    request_render_update_ = true;
}

} // namespace oid
