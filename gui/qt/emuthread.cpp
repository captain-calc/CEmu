#include "emuthread.h"

#include <cassert>
#include <cstdarg>
#include <thread>

#include "mainwindow.h"

#include "capture/animated-png.h"
#include "../../core/emu.h"
#include "../../core/cpu.h"
#include "../../core/control.h"
#include "../../core/link.h"

EmuThread *emu_thread = Q_NULLPTR;

void gui_emu_sleep(unsigned long microseconds) {
    QThread::usleep(microseconds);
}

void gui_do_stuff(void) {
    emu_thread->doStuff();
}

void gui_console_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    emu_thread->writeConsoleBuffer(CONSOLE_NORM, format, args);
    va_end(args);
}

void gui_console_err_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    emu_thread->writeConsoleBuffer(CONSOLE_ERR, format, args);
    va_end(args);
}

void gui_debugger_send_command(int reason, uint32_t addr) {
    emu_thread->sendDebugCommand(reason, addr);
}

void gui_debugger_raise_or_disable(bool entered) {
    if (entered) {
        emu_thread->raiseDebugger();
    } else {
        emu_thread->disableDebugger();
    }
}

void throttle_timer_wait(void) {
    emu_thread->throttleTimerWait();
}

EmuThread::EmuThread(QObject *p) : QThread(p), consoleWriteSemaphore(CONSOLE_BUFFER_SIZE) {
    assert(emu_thread == Q_NULLPTR);
    emu_thread = this;
    speed = actualSpeed = 100;
    lastTime = std::chrono::steady_clock::now();
}

void EmuThread::writeConsoleBuffer(int dest, const char *format, va_list args) {
    va_list argsCopy;
    va_copy(argsCopy, args);
    int available = consoleWriteSemaphore.available();
    int remaining = CONSOLE_BUFFER_SIZE - consoleWritePosition;
    int space = available < remaining ? available : remaining;
    int size = vsnprintf(consoleBuffer + consoleWritePosition, space, format, argsCopy);
    va_end(argsCopy);
    if (size < space) {
        if (size > 0) {
            consoleWriteSemaphore.acquire(size);
            consoleWritePosition += size;
            consoleReadSemaphore.release(size);
            emit consoleStr(dest);
        }
    } else {
        int bufferPosition = 0;
        char *buffer = size < available - remaining ? consoleBuffer : new char[size + 1];
        if (buffer && vsnprintf(buffer, size + 1, format, args) >= 0) {
            while (size >= remaining) {
                consoleWriteSemaphore.acquire(remaining);
                memcpy(consoleBuffer + consoleWritePosition, buffer + bufferPosition, remaining);
                bufferPosition += remaining;
                size -= remaining;
                consoleWritePosition = 0;
                consoleReadSemaphore.release(remaining);
                remaining = CONSOLE_BUFFER_SIZE;
                emit consoleStr(dest);
            }
            if (size) {
                consoleWriteSemaphore.acquire(size);
                memmove(consoleBuffer + consoleWritePosition, buffer + bufferPosition, size);
                consoleWritePosition += size;
                consoleReadSemaphore.release(size);
                emit consoleStr(dest);
            }
        }
        if (buffer != consoleBuffer) {
            delete [] buffer;
        }
    }
}

void EmuThread::reset() {
    doReset = true;
}

void EmuThread::setEmuSpeed(int value) {
    speed = value;
}

void EmuThread::setThrottleMode(bool throttled) {
    throttleOn = throttled;
}

void EmuThread::debug(bool state) {
    enterDebugger = state;
    if (inDebugger && !state) {
        debug_clear_temp_break();
        close_debugger();
    }
}

void EmuThread::send(const QStringList &list, unsigned int location) {
    enterSendState = true;
    vars = list;
    sendLoc = location;
}

void EmuThread::receive() {
    enterReceiveState = true;
}

void EmuThread::unlock() {
    mutex.lock();
    cv.notify_all();
    mutex.unlock();
}

// Called occasionally, only way to do something in the same thread the emulator runs in.
void EmuThread::doStuff() {
    const std::chrono::steady_clock::time_point cur_time = std::chrono::steady_clock::now();
    lastTime += std::chrono::steady_clock::now() - cur_time;

    if (doReset) {
        cpu.events |= EVENT_RESET;
        doReset = false;
    }

    if (enterSave) {
        bool success = emu_save(saveImage, savePath.toStdString().c_str());
        emit saved(success);
        enterSave = false;
    }

    if (enterSendState) {
        sendFiles();
        enterSendState = false;
    }

    if (enterReceiveState) {
        std::unique_lock<std::mutex> lock(mutex);
        emit receiveReady();
        cv.wait(lock);
        enterReceiveState = false;
    }

    if (enterDebugger) {
        open_debugger(DBG_USER, 0);
        enterDebugger = false;
    }
}

void EmuThread::sendFiles() {
    const int fileNum = vars.size();

    for (int i = 0; i < fileNum; i++) {
        const QString &f = vars.at(i);
        emit sentFile(f, sendVariableLink(f.toUtf8(), sendLoc));
    }

    emit sentFile(QString(), LINK_GOOD);
}

void EmuThread::setActualSpeed(int value) {
    if (!control.off) {
        if (actualSpeed != value) {
            actualSpeed = value;
            emit actualSpeedChanged(value);
        }
    }
}

void EmuThread::throttleTimerWait() {
    if (!speed) {
        setActualSpeed(0);
        while(!speed) {
            QThread::usleep(10000);
        }
        return;
    }
    std::chrono::duration<int, std::ratio<100, 60>> unit(1);
    std::chrono::steady_clock::duration interval(std::chrono::duration_cast<std::chrono::steady_clock::duration>
                                                (std::chrono::duration<int, std::ratio<1, 60 * 1000000>>(1000000 * 100 / speed)));
    std::chrono::steady_clock::time_point cur_time = std::chrono::steady_clock::now(), next_time = lastTime + interval;
    if (throttleOn && cur_time < next_time) {
        setActualSpeed(speed);
        lastTime = next_time;
        std::this_thread::sleep_until(next_time);
    } else {
        if (lastTime != cur_time) {
            setActualSpeed(unit / (cur_time - lastTime));
            lastTime = cur_time;
        }
        std::this_thread::yield();
    }
}

void EmuThread::run() {
    emu_loop();
    emit stopped();
}

int EmuThread::load(bool restore, const QString &rom, const QString &image) {
    int ret = EMU_LOAD_FAIL;

    setTerminationEnabled();

    if (!stop()) {
        return EMU_LOAD_FAIL;
    }

    if (restore) {
        ret = emu_load(true, image.toStdString().c_str());
    } else {
        ret = emu_load(false, rom.toStdString().c_str());
    }
    return ret;
}

bool EmuThread::stop() {
    if (!this->isRunning()) {
        return true;
    }

    lcd_gui_callback = NULL;
    lcd_gui_callback_data = NULL;
    exiting = true;
    cpu.next = 0;

    if (!this->wait(200)) {
        terminate();
        if (!this->wait(200)) {
            return false;
        }
    }

    return true;
}

void EmuThread::save(bool image, const QString &path) {
    savePath = path;
    saveImage = image;
    enterSave = true;
}
