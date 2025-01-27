/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include <errno.h>
#include <time.h>
#include <optional>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>
#include <Common/Exception.h>
#include <Common/CurrentMetrics.h>
#include <IO/ReadBufferFromFileDescriptor.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteHelpers.h>
#include <sys/stat.h>
#include <Common/UnicodeBar.h>
#include <Common/TerminalSize.h>
#include <IO/Operators.h>
#include <Interpreters/Context.h>

#if defined(__clang__) && __clang_major__ >= 13
#pragma clang diagnostic ignored "-Wreserved-identifier"
#endif

namespace ProfileEvents
{
    extern const Event ReadBufferFromFileDescriptorRead;
    extern const Event ReadBufferFromFileDescriptorReadFailed;
    extern const Event ReadBufferFromFileDescriptorReadBytes;
    extern const Event DiskReadElapsedMicroseconds;
    extern const Event Seek;
}

namespace CurrentMetrics
{
    extern const Metric Read;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_READ_FROM_FILE_DESCRIPTOR;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int CANNOT_SEEK_THROUGH_FILE;
    extern const int CANNOT_SELECT;
    extern const int CANNOT_FSTAT;
}


std::string ReadBufferFromFileDescriptor::getFileName() const
{
    return "(fd = " + toString(fd) + ")";
}


bool ReadBufferFromFileDescriptor::nextImpl()
{
    size_t bytes_read = readInto(internal_buffer.begin(), internal_buffer.size());

    if (bytes_read)
    {
        working_buffer = internal_buffer;
        working_buffer.resize(bytes_read);
        return true;
    }
    return false;
}


/// If 'offset' is small enough to stay in buffer after seek, then true seek in file does not happen.
off_t ReadBufferFromFileDescriptor::seek(off_t offset, int whence)
{
    size_t new_pos;
    if (whence == SEEK_SET)
    {
        assert(offset >= 0);
        new_pos = offset;
    }
    else if (whence == SEEK_CUR)
    {
        new_pos = file_offset_of_buffer_end - (working_buffer.end() - pos) + offset;
    }
    else
    {
        throw Exception("ReadBufferFromFileDescriptor::seek expects SEEK_SET or SEEK_CUR as whence", ErrorCodes::ARGUMENT_OUT_OF_BOUND);
    }

    /// Position is unchanged.
    if (new_pos + (working_buffer.end() - pos) == file_offset_of_buffer_end)
        return new_pos;

    // file_offset_of_buffer_end corresponds to working_buffer.end(); it's a past-the-end pos,
    // so the second inequality is strict.
    if (file_offset_of_buffer_end - working_buffer.size() <= static_cast<size_t>(new_pos)
        && new_pos < file_offset_of_buffer_end)
    {
        /// Position is still inside buffer.
        pos = working_buffer.end() - file_offset_of_buffer_end + new_pos;
        assert(pos >= working_buffer.begin());
        assert(pos < working_buffer.end());

        return new_pos;
    }
    else
    {
        ProfileEvents::increment(ProfileEvents::Seek);
        Stopwatch watch(profile_callback ? clock_type : CLOCK_MONOTONIC);

        pos = working_buffer.end();
        off_t res = ::lseek(fd, new_pos, SEEK_SET);
        if (-1 == res)
            throwFromErrnoWithPath("Cannot seek through file " + getFileName(), getFileName(),
                ErrorCodes::CANNOT_SEEK_THROUGH_FILE);
        file_offset_of_buffer_end = new_pos;

        watch.stop();
        ProfileEvents::increment(ProfileEvents::DiskReadElapsedMicroseconds, watch.elapsedMicroseconds());

        return res;
    }
}


void ReadBufferFromFileDescriptor::rewind()
{
    ProfileEvents::increment(ProfileEvents::Seek);
    off_t res = ::lseek(fd, 0, SEEK_SET);
    if (-1 == res)
        throwFromErrnoWithPath("Cannot seek through file " + getFileName(), getFileName(),
            ErrorCodes::CANNOT_SEEK_THROUGH_FILE);

    /// Clearing the buffer with existing data. New data will be read on subsequent call to 'next'.
    working_buffer.resize(0);
    pos = working_buffer.begin();
}


/// Assuming file descriptor supports 'select', check that we have data to read or wait until timeout.
bool ReadBufferFromFileDescriptor::poll(size_t timeout_microseconds)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval timeout = { time_t(timeout_microseconds / 1000000), suseconds_t(timeout_microseconds % 1000000) };

    int res = select(1, &fds, nullptr, nullptr, &timeout);

    if (-1 == res)
        throwFromErrno("Cannot select", ErrorCodes::CANNOT_SELECT);

    return res > 0;
}


off_t ReadBufferFromFileDescriptor::size()
{
    struct stat buf;
    int res = fstat(fd, &buf);
    if (-1 == res)
        throwFromErrnoWithPath("Cannot execute fstat " + getFileName(), getFileName(), ErrorCodes::CANNOT_FSTAT);
    return buf.st_size;
}


void ReadBufferFromFileDescriptor::setProgressCallback(ContextPtr context)
{
    auto file_progress_callback = context->getFileProgressCallback();

    if (!file_progress_callback)
        return;

    setProfileCallback([file_progress_callback](const ProfileInfo & progress)
    {
        file_progress_callback(FileProgress(progress.bytes_read, 0));
    });
}

size_t ReadBufferFromFileDescriptor::readBig(char * to, size_t n)
{
    /// Read from current working buffer if possible
    size_t read_bytes = 0;
    if (size_t remain = available(); remain > 0)
    {
        read_bytes = std::min(n, remain);

        memcpy(to, pos, read_bytes);
        pos += read_bytes;

        if (read_bytes >= n)
        {
            bytes += read_bytes;
            return n;
        }
    }

    /// Already drain current working buffer
    resetWorkingBuffer();

    while (read_bytes < n)
    {
        size_t readed = readInto(to + read_bytes, n - read_bytes);
        if (readed == 0)
        {
            break;
        }

        read_bytes += readed;
    }
    bytes += read_bytes;
    return read_bytes;
}

size_t ReadBufferFromFileDescriptor::readInto(char * to, size_t n)
{
    size_t bytes_read = 0;
    while (!bytes_read)
    {
        ProfileEvents::increment(ProfileEvents::ReadBufferFromFileDescriptorRead);

        Stopwatch watch(profile_callback ? clock_type : CLOCK_MONOTONIC);

        ssize_t res = 0;
        {
            CurrentMetrics::Increment metric_increment{CurrentMetrics::Read};

            res = ::read(fd, to, n);
        }
        if (!res)
            break;

        if (-1 == res && errno != EINTR)
        {
            ProfileEvents::increment(ProfileEvents::ReadBufferFromFileDescriptorReadFailed);
            throwFromErrnoWithPath("Cannot read from file " + getFileName(), getFileName(),
                                   ErrorCodes::CANNOT_READ_FROM_FILE_DESCRIPTOR);
        }

        if (res > 0)
            bytes_read += res;

        /// It reports real time spent including the time spent while thread was preempted doing nothing.
        /// And it is Ok for the purpose of this watch (it is used to lower the number of threads to read from tables).
        /// Sometimes it is better to use taskstats::blkio_delay_total, but it is quite expensive to get it
        /// (TaskStatsInfoGetter has about 500K RPS).
        watch.stop();
        ProfileEvents::increment(ProfileEvents::DiskReadElapsedMicroseconds, watch.elapsedMicroseconds());

        if (profile_callback)
        {
            ProfileInfo info;
            info.bytes_requested = n;
            info.bytes_read = res;
            info.nanoseconds = watch.elapsed();
            profile_callback(info);
        }
    }

    file_offset_of_buffer_end += bytes_read;

    if (bytes_read)
    {
        ProfileEvents::increment(ProfileEvents::ReadBufferFromFileDescriptorReadBytes, bytes_read);
    }
    return bytes_read;
}

}
