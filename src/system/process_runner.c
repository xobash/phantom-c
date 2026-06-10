#define _POSIX_C_SOURCE 200809L
#include "phantom/process_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static void append_output(ph_process_result *result, const char *buf, size_t got) {
    size_t used = strlen(result->output);
    if (used + 1 >= sizeof result->output) return;
    size_t room = sizeof result->output - used - 1;
    if (got > room) got = room;
    memcpy(result->output + used, buf, got);
    result->output[used + got] = '\0';
}

#ifdef _WIN32
static wchar_t *utf8_to_wide_command(const char *command) {
    int len = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
    if (len <= 0) len = MultiByteToWideChar(CP_ACP, 0, command, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)len, sizeof *wide);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, command, -1, wide, len) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, command, -1, wide, len) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static void drain_pipe(HANDLE pipe, ph_process_result *result) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) || available == 0) break;
        char chunk[512];
        DWORD want = available < sizeof chunk ? available : (DWORD)sizeof chunk;
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, want, &got, NULL) || got == 0) break;
        append_output(result, chunk, (size_t)got);
    }
}

static HANDLE create_kill_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}
#endif

#ifndef _WIN32
static long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
#endif

bool ph_process_run(const char *command, int timeout_seconds, ph_process_result *result, ph_error *err) {
    return ph_process_run_input(command, NULL, timeout_seconds, result, err);
}

bool ph_process_run_input(const char *command, const char *input, int timeout_seconds, ph_process_result *result, ph_error *err) {
    if (!command || !result) {
        ph_error_set(err, 1, "missing process command");
        return false;
    }
    memset(result, 0, sizeof *result);
    if (timeout_seconds <= 0) timeout_seconds = 1;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result->exit_code = 127;
        ph_error_set(err, 127, "failed to create process pipe: win32=%lu", (unsigned long)GetLastError());
        return false;
    }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result->exit_code = 127;
        ph_error_set(err, 127, "failed to protect process pipe handle: win32=%lu", (unsigned long)GetLastError());
        return false;
    }

    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    if (input) {
        /* Generous buffer so writing the script cannot deadlock against
         * a child that emits output before reading stdin. */
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 1u << 20) ||
            !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
            if (stdin_read) CloseHandle(stdin_read);
            if (stdin_write) CloseHandle(stdin_write);
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            result->exit_code = 127;
            ph_error_set(err, 127, "failed to create stdin pipe: win32=%lu", (unsigned long)GetLastError());
            return false;
        }
    }

    wchar_t *wide_command = utf8_to_wide_command(command);
    if (!wide_command) {
        if (stdin_read) CloseHandle(stdin_read);
        if (stdin_write) CloseHandle(stdin_write);
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result->exit_code = 127;
        ph_error_set(err, 127, "failed to encode process command");
        return false;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = input ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);

    BOOL created = CreateProcessW(NULL, wide_command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD create_error = GetLastError();
    free(wide_command);
    CloseHandle(write_pipe);
    if (stdin_read) CloseHandle(stdin_read);
    if (!created) {
        if (stdin_write) CloseHandle(stdin_write);
        CloseHandle(read_pipe);
        result->exit_code = 127;
        ph_error_set(err, 127, "failed to start process: win32=%lu", (unsigned long)create_error);
        return false;
    }
    if (stdin_write) {
        const char *p = input;
        size_t remaining = strlen(input);
        while (remaining > 0) {
            DWORD wrote = 0;
            if (!WriteFile(stdin_write, p, remaining > 65536 ? 65536 : (DWORD)remaining, &wrote, NULL) || wrote == 0) break;
            p += wrote;
            remaining -= wrote;
        }
        CloseHandle(stdin_write);
    }

    HANDLE job = create_kill_job();
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_seconds * 1000ULL;
    bool process_done = false;
    for (;;) {
        drain_pipe(read_pipe, result);
        DWORD wait_ms = 100;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            result->timed_out = true;
            result->exit_code = 124;
            if (job) (void)TerminateJobObject(job, 124);
            else (void)TerminateProcess(pi.hProcess, 124);
            (void)WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        ULONGLONG remain = deadline - now;
        if (remain < wait_ms) wait_ms = (DWORD)remain;
        DWORD waited = WaitForSingleObject(pi.hProcess, wait_ms);
        if (waited == WAIT_OBJECT_0) {
            process_done = true;
            break;
        }
        if (waited == WAIT_FAILED) {
            result->exit_code = 127;
            ph_error_set(err, 127, "failed waiting for process: win32=%lu", (unsigned long)GetLastError());
            (void)TerminateProcess(pi.hProcess, 127);
            break;
        }
    }

    drain_pipe(read_pipe, result);
    CloseHandle(read_pipe);

    if (process_done) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) result->exit_code = (int)exit_code;
        else result->exit_code = 127;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    if (result->timed_out) {
        ph_error_set(err, 124, "process timed out after %d seconds", timeout_seconds);
        return false;
    }
    if (result->exit_code != 0) {
        ph_error_set(err, result->exit_code, "process exited with code %d", result->exit_code);
        return false;
    }
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        ph_error_set(err, 127, "failed to create process pipe");
        result->exit_code = 127;
        return false;
    }
    int stdin_fd[2] = {-1, -1};
    if (input && pipe(stdin_fd) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        ph_error_set(err, 127, "failed to create stdin pipe");
        result->exit_code = 127;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (input) { close(stdin_fd[0]); close(stdin_fd[1]); }
        ph_error_set(err, 127, "failed to fork process");
        result->exit_code = 127;
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (input) {
            close(stdin_fd[1]);
            dup2(stdin_fd[0], STDIN_FILENO);
            close(stdin_fd[0]);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    if (input) {
        close(stdin_fd[0]);
        signal(SIGPIPE, SIG_IGN); /* a fast-exiting child must not kill us */
        const char *p = input;
        size_t remaining = strlen(input);
        while (remaining > 0) {
            ssize_t wrote = write(stdin_fd[1], p, remaining);
            if (wrote <= 0) break;
            p += wrote;
            remaining -= (size_t)wrote;
        }
        close(stdin_fd[1]);
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    long deadline = monotonic_ms() + (long)timeout_seconds * 1000L;
    int status = 0;
    bool child_done = false;
    for (;;) {
        char chunk[512];
        ssize_t got;
        while ((got = read(pipefd[0], chunk, sizeof chunk)) > 0) append_output(result, chunk, (size_t)got);

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { child_done = true; break; }
        if (w < 0 && errno != EINTR) { result->exit_code = 127; break; }

        long now = monotonic_ms();
        if (now >= deadline) {
            result->timed_out = true;
            result->exit_code = 124;
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            break;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(pipefd[0], &set);
        struct timeval tv;
        long remain = deadline - now;
        if (remain > 100) remain = 100;
        if (remain < 1) remain = 1;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (int)(remain % 1000) * 1000;
        (void)select(pipefd[0] + 1, &set, NULL, NULL, &tv);
    }

    char chunk[512];
    ssize_t got;
    while ((got = read(pipefd[0], chunk, sizeof chunk)) > 0) append_output(result, chunk, (size_t)got);
    close(pipefd[0]);

    if (child_done) {
        if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result->exit_code = 128 + WTERMSIG(status);
        else result->exit_code = status;
    }

    if (result->timed_out) {
        ph_error_set(err, 124, "process timed out after %d seconds", timeout_seconds);
        return false;
    }
    if (result->exit_code != 0) {
        ph_error_set(err, result->exit_code, "process exited with code %d", result->exit_code);
        return false;
    }
    return true;
#endif
}
