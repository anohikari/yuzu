#include <QHBoxLayout>
#include <QKeyEvent>
#include <QApplication>

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
// Required for screen DPI information
#include <QScreen>
#include <QWindow>
#endif

#include "common/common.h"
#include "bootmanager.hxx"

#include "core/core.h"
#include "core/settings.h"

#include "video_core/video_core.h"

#include "citra_qt/version.h"

#define APP_NAME        "citra"
#define APP_VERSION     "0.1-" VERSION
#define APP_TITLE       APP_NAME " " APP_VERSION
#define COPYRIGHT       "Copyright (C) 2013-2014 Citra Team"

EmuThread::EmuThread(GRenderWindow* render_window) : 
    filename(""), exec_cpu_step(false), cpu_running(false),
    stop_run(false), render_window(render_window)
{
}

void EmuThread::SetFilename(std::string filename)
{
    this->filename = filename;
}

void EmuThread::run()
{
    stop_run = false;
    while (!stop_run)
    {
        if (cpu_running)
        {
            Core::RunLoop();
        } 
        else if (exec_cpu_step)
        {
            exec_cpu_step = false;
            Core::SingleStep();
            emit CPUStepped();
            yieldCurrentThread();
        }
    }
    render_window->moveContext();

    Core::Stop();
}

void EmuThread::Stop()
{
    if (!isRunning())
    {
        INFO_LOG(MASTER_LOG, "EmuThread::Stop called while emu thread wasn't running, returning...");
        return;
    }
    stop_run = true;

    //core::g_state = core::SYS_DIE;

    wait(500);
    if (isRunning())
    {
        WARN_LOG(MASTER_LOG, "EmuThread still running, terminating...");
        quit();
        wait(1000);
        if (isRunning())
        {
            WARN_LOG(MASTER_LOG, "EmuThread STILL running, something is wrong here...");
            terminate();
        }
    }
    INFO_LOG(MASTER_LOG, "EmuThread stopped");
}


// This class overrides paintEvent and resizeEvent to prevent the GUI thread from stealing GL context.
// The corresponding functionality is handled in EmuThread instead
class GGLWidgetInternal : public QGLWidget
{
public:
    GGLWidgetInternal(QGLFormat fmt, GRenderWindow* parent) : QGLWidget(fmt, parent)
    {
        parent_ = parent;
    }

    void paintEvent(QPaintEvent* ev) override
    {
    }
    void resizeEvent(QResizeEvent* ev) override {
        parent_->SetClientAreaWidth(size().width());
        parent_->SetClientAreaHeight(size().height());
    }
private:
    GRenderWindow* parent_;
};

EmuThread& GRenderWindow::GetEmuThread()
{
    return emu_thread;
}

GRenderWindow::GRenderWindow(QWidget* parent) : QWidget(parent), emu_thread(this), keyboard_id(0)
{
    keyboard_id = KeyMap::NewDeviceId();
    ReloadSetKeymaps();

    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground, WA_DontShowOnScreen, WA_DeleteOnClose
    QGLFormat fmt;
    fmt.setVersion(3,2);
    fmt.setProfile(QGLFormat::CoreProfile);
    // Requests a forward-compatible context, which is required to get a 3.2+ context on OS X
    fmt.setOption(QGL::NoDeprecatedFunctions);
    
    child = new GGLWidgetInternal(fmt, this);
    QBoxLayout* layout = new QHBoxLayout(this);
    resize(VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight + VideoCore::kScreenBottomHeight);
    layout->addWidget(child);
    layout->setMargin(0);
    setLayout(layout);
    QObject::connect(&emu_thread, SIGNAL(started()), this, SLOT(moveContext()));

    BackupGeometry();
}

void GRenderWindow::moveContext()
{
    DoneCurrent();
    // We need to move GL context to the swapping thread in Qt5
#if QT_VERSION > QT_VERSION_CHECK(5, 0, 0)
    // If the thread started running, move the GL Context to the new thread. Otherwise, move it back.
    child->context()->moveToThread((QThread::currentThread() == qApp->thread()) ? &emu_thread : qApp->thread());
#endif
}

GRenderWindow::~GRenderWindow()
{
    if (emu_thread.isRunning())
        emu_thread.Stop();
}

void GRenderWindow::SwapBuffers()
{
    // MakeCurrent is already called in renderer_opengl
    child->swapBuffers();
}

void GRenderWindow::closeEvent(QCloseEvent* event)
{
    if (emu_thread.isRunning())
        emu_thread.Stop();
    QWidget::closeEvent(event);
}

void GRenderWindow::MakeCurrent()
{
    child->makeCurrent();
}

void GRenderWindow::DoneCurrent()
{
    child->doneCurrent();
}

void GRenderWindow::PollEvents() {
    // TODO(ShizZy): Does this belong here? This is a reasonable place to update the window title
    //  from the main thread, but this should probably be in an event handler...
    /*
    static char title[128];
    sprintf(title, "%s (FPS: %02.02f)", window_title_.c_str(), 
        video_core::g_renderer->current_fps());
    setWindowTitle(title);
    */
}

// On Qt 5.1+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::GetFramebufferSize(int* fbWidth, int* fbHeight)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 1, 0)
    int pixelRatio = child->QPaintDevice::devicePixelRatio();
    
    *fbWidth = child->QPaintDevice::width() * pixelRatio;
    *fbHeight = child->QPaintDevice::height() * pixelRatio;
#else
    *fbWidth = child->QPaintDevice::width();
    *fbHeight = child->QPaintDevice::height();
#endif
}

void GRenderWindow::BackupGeometry()
{
    geometry = ((QGLWidget*)this)->saveGeometry();
}

void GRenderWindow::RestoreGeometry()
{
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry)
{
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry()
{
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == NULL)
        return ((QGLWidget*)this)->saveGeometry();
    else
        return geometry;
}

void GRenderWindow::keyPressEvent(QKeyEvent* event)
{
    EmuWindow::KeyPressed({event->key(), keyboard_id});
    HID_User::PadUpdateComplete();
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event)
{
    EmuWindow::KeyReleased({event->key(), keyboard_id});
    HID_User::PadUpdateComplete();
}

void GRenderWindow::ReloadSetKeymaps()
{
    KeyMap::SetKeyMapping({Settings::values.pad_a_key,      keyboard_id}, HID_User::PAD_A);
    KeyMap::SetKeyMapping({Settings::values.pad_b_key,      keyboard_id}, HID_User::PAD_B);
    KeyMap::SetKeyMapping({Settings::values.pad_select_key, keyboard_id}, HID_User::PAD_SELECT);
    KeyMap::SetKeyMapping({Settings::values.pad_start_key,  keyboard_id}, HID_User::PAD_START);
    KeyMap::SetKeyMapping({Settings::values.pad_dright_key, keyboard_id}, HID_User::PAD_RIGHT);
    KeyMap::SetKeyMapping({Settings::values.pad_dleft_key,  keyboard_id}, HID_User::PAD_LEFT);
    KeyMap::SetKeyMapping({Settings::values.pad_dup_key,    keyboard_id}, HID_User::PAD_UP);
    KeyMap::SetKeyMapping({Settings::values.pad_ddown_key,  keyboard_id}, HID_User::PAD_DOWN);
    KeyMap::SetKeyMapping({Settings::values.pad_r_key,      keyboard_id}, HID_User::PAD_R);
    KeyMap::SetKeyMapping({Settings::values.pad_l_key,      keyboard_id}, HID_User::PAD_L);
    KeyMap::SetKeyMapping({Settings::values.pad_x_key,      keyboard_id}, HID_User::PAD_X);
    KeyMap::SetKeyMapping({Settings::values.pad_y_key,      keyboard_id}, HID_User::PAD_Y);
    KeyMap::SetKeyMapping({Settings::values.pad_sright_key, keyboard_id}, HID_User::PAD_CIRCLE_RIGHT);
    KeyMap::SetKeyMapping({Settings::values.pad_sleft_key,  keyboard_id}, HID_User::PAD_CIRCLE_LEFT);
    KeyMap::SetKeyMapping({Settings::values.pad_sup_key,    keyboard_id}, HID_User::PAD_CIRCLE_UP);
    KeyMap::SetKeyMapping({Settings::values.pad_sdown_key,  keyboard_id}, HID_User::PAD_CIRCLE_DOWN);
}

