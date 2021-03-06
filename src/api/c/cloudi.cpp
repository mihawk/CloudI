// -*- coding: utf-8; Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*-
// ex: set softtabstop=4 tabstop=4 shiftwidth=4 expandtab fileencoding=utf-8:
//
// BSD LICENSE
// 
// Copyright (c) 2011-2012, Michael Truog <mjtruog at gmail dot com>
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in
//       the documentation and/or other materials provided with the
//       distribution.
//     * All advertising materials mentioning features or use of this
//       software must display the following acknowledgment:
//         This product includes software developed by Michael Truog
//     * The name of the author may not be used to endorse or promote
//       products derived from this software without specific prior
//       written permission
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
// CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.
//

#include "cloudi.hpp"
#undef CLOUDI_HPP // avoid header warning
#include "cloudi.h"
#define CLOUDI_HPP
#include "realloc_ptr.hpp"
#include "copy_ptr.hpp"
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <ei.h>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <string>
#include <list>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "assert.hpp"

namespace
{
    class callback_function
    {
        private:
            class callback_function_c :
                public CloudI::API::callback_function_generic
            {
                public:
                    callback_function_c(cloudi_instance_t * p,
                                        cloudi_callback_t f) :
                        m_p(p), m_f(f) {}
                    virtual ~callback_function_c() throw() {}

                    virtual void operator () (int const command,
                                              char const * const name,
                                              char const * const pattern,
                                              void const * const request_info,
                                              uint32_t const request_info_size,
                                              void const * const request,
                                              uint32_t const request_size,
                                              uint32_t timeout,
                                              int8_t priority,
                                              char const * const trans_id,
                                              char const * const pid,
                                              uint32_t const pid_size)
                    {
                        m_f(m_p,
                            command,
                            name,
                            pattern,
                            request_info,
                            request_info_size,
                            request,
                            request_size,
                            timeout,
                            priority,
                            trans_id,
                            pid,
                            pid_size);
                    }
                private:
                    cloudi_instance_t * m_p;
                    cloudi_callback_t m_f;
            };

        public:
            callback_function(CloudI::API::callback_function_generic * p) :
                m_function(p) {}

            callback_function(cloudi_instance_t * p,
                              cloudi_callback_t f) :
                m_function(new callback_function_c(p, f)) {}

            void operator () (int const command,
                              char const * const name,
                              char const * const pattern,
                              void const * const request_info,
                              uint32_t const request_info_size,
                              void const * const request,
                              uint32_t const request_size,
                              uint32_t timeout,
                              int8_t const priority,
                              char const * const trans_id,
                              char const * const pid,
                              uint32_t const pid_size) const
            {
                (*m_function)(command,
                              name,
                              pattern,
                              request_info,
                              request_info_size,
                              request,
                              request_size,
                              timeout,
                              priority,
                              trans_id,
                              pid,
                              pid_size);
            }
                
        private:
            boost::shared_ptr<CloudI::API::callback_function_generic>
                m_function;
    };

    class callback_function_lookup
    {
        private:
            class callback_function_queue
            {
                private:
                    typedef std::list<callback_function> queue_t;
                public:
                    callback_function_queue(callback_function const & f) :
                        m_queue(new queue_t()),
                        m_size(1)
                    {
                        m_queue->push_back(f);
                    }

                    void push_back(callback_function const & f)
                    {
                        m_queue->push_back(f);
                        m_size++;
                    }

                    callback_function const & cycle()
                    {
                        queue_t & queue = *m_queue;
                        if (m_size == 1)
                            return queue.front();
                        queue.push_back(queue.front());
                        queue.pop_front();
                        return queue.back();
                    }
                private:
                    boost::shared_ptr<queue_t> m_queue;
                    size_t m_size;
            };

            typedef boost::unordered_map<std::string, callback_function_queue>
                lookup_queue_t;
            typedef std::pair<std::string, callback_function_queue>
                lookup_queue_pair_t;
        public:
            void insert(std::string const & pattern,
                        callback_function const & f)
            {
                lookup_queue_t::iterator itr = m_lookup.find(pattern);
                if (itr == m_lookup.end())
                {
                    m_lookup.insert(lookup_queue_pair_t(pattern, f));
                }
                else
                {
                    itr->second.push_back(f);
                }
            }

            bool erase(std::string const & pattern)
            {
                lookup_queue_t::iterator itr = m_lookup.find(pattern);
                if (itr == m_lookup.end())
                    return false;
                m_lookup.erase(itr);
                return true;
            }

            callback_function find(std::string const & pattern)
            {
                lookup_queue_t::iterator itr = m_lookup.find(pattern);
                assert(itr != m_lookup.end());
                return itr->second.cycle();
            }

        private:
            lookup_queue_t m_lookup;
            
    };
    typedef callback_function_lookup lookup_t;
    typedef realloc_ptr<char> buffer_t;

    int errno_read()
    {
        switch (errno)
        {
            case EAGAIN:
                return cloudi_error_read_EAGAIN;
            case EBADF:
                return cloudi_error_read_EBADF;
            case EFAULT:
                return cloudi_error_read_EFAULT;
            case EINTR:
                return cloudi_error_read_EINTR;
            case EINVAL:
                return cloudi_error_read_EINVAL;
            case EIO:
                return cloudi_error_read_EIO;
            case EISDIR:
                return cloudi_error_read_EISDIR;
            default:
                return cloudi_error_read_unknown;
        }
    }

    int errno_write()
    {
        switch (errno)
        {
            case EAGAIN:
                return cloudi_error_write_EAGAIN;
            case EBADF:
                return cloudi_error_write_EBADF;
            case EFAULT:
                return cloudi_error_write_EFAULT;
            case EFBIG:
                return cloudi_error_write_EFBIG;
            case EINTR:
                return cloudi_error_write_EINTR;
            case EINVAL:
                return cloudi_error_write_EINVAL;
            case EIO:
                return cloudi_error_write_EIO;
            case ENOSPC:
                return cloudi_error_write_ENOSPC;
            case EPIPE:
                return cloudi_error_write_EPIPE;
            default:
                return cloudi_error_write_unknown;
        }
    }

    int errno_poll()
    {
        switch (errno)
        {
            case EBADF:
                return cloudi_error_poll_EBADF;
            case EFAULT:
                return cloudi_error_poll_EFAULT;
            case EINTR:
                return cloudi_error_poll_EINTR;
            case EINVAL:
                return cloudi_error_poll_EINVAL;
            case ENOMEM:
                return cloudi_error_poll_ENOMEM;
            default:
                return cloudi_error_poll_unknown;
        }
    }

    int data_ready(int fd, bool & ready)
    {
        struct pollfd fds[1] = {{fd, POLLIN | POLLPRI, 0}};
        int const count = poll(fds, 1, 0);
        if (count == -1)
            return errno_poll();
        ready = (count == 1);
        return cloudi_success;
    }

    int read_exact(int fd,
                   unsigned char * const buffer,
                   uint32_t const length)
    {
        uint32_t total = 0;
        while (total < length)
        {
            ssize_t const i = ::read(fd, buffer + total, length - total);
            if (i <= 0)
            {
                if (i == -1)
                    return errno_read();
                else
                    return cloudi_error_read_null;
            }
            total += i;
        }
        if (total > length)
            return cloudi_error_read_overflow;
        return cloudi_success;
    }

    int read_all(int fd, int const use_header,
                 buffer_t & buffer, uint32_t & total,
                 uint32_t const buffer_size)
    {
        total = 0;
        if (use_header)
        {
            unsigned char header[4];
            int const status = read_exact(fd, header, 4);
            if (status)
                return status;
            uint32_t const length = (header[0] << 24) |
                                    (header[1] << 16) |
                                    (header[2] <<  8) |
                                     header[3];
            if (buffer.reserve(length) == false)
                return cloudi_out_of_memory;
            total = length;
            return read_exact(fd, buffer.get<unsigned char>(), length);
        }
        else
        {
            bool ready = true;
            while (ready)
            {
                if (buffer.reserve(total + buffer_size) == false)
                    return cloudi_out_of_memory;
                ssize_t i = ::read(fd, &buffer[total], buffer_size);
                if (i < 0)
                    return errno_read();
                total += i;
                ready = (i == static_cast<signed>(buffer_size)) ||
                        (i == 0 && total == 0);
                if (ready)
                {
                    int const status = data_ready(fd, ready);
                    if (status)
                        return status;
                }
            }
        }

        return cloudi_success;
    }

    int write_exact(int fd, int const use_header,
                    char * const buffer, uint32_t const length)
    {
        if (use_header)
        {
            uint32_t const length_body = length - 4;
            buffer[0] = (length_body & 0xff000000) >> 24;
            buffer[1] = (length_body & 0x00ff0000) >> 16;
            buffer[2] = (length_body & 0x0000ff00) >> 8;
            buffer[3] =  length_body & 0x000000ff;
        }

        uint32_t total = 0;
        while (total < length)
        {
            ssize_t const i = ::write(fd, buffer + total, length - total);
            if (i <= 0)
            {
                if (i == -1)
                    return errno_write();
                else
                    return cloudi_error_write_null;
            }
            total += i;
        }
        if (total > length)
            return cloudi_error_write_overflow;
        return cloudi_success;
    }

} // anonymous namespace

extern "C" {

static void exit_handler()
{
    ::fflush(stdout);
    ::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();
    std::clog.flush();
}

int cloudi_initialize(cloudi_instance_t * p,
                      unsigned int const thread_index)
{
    char const * const protocol = ::getenv("CLOUDI_API_INIT_PROTOCOL");
    if (protocol == 0)
        return cloudi_invalid_input;
    char const * const buffer_size_p = ::getenv("CLOUDI_API_INIT_BUFFER_SIZE");
    if (buffer_size_p == 0)
        return cloudi_invalid_input;
    uint32_t const buffer_size = ::atoi(buffer_size_p);
    p->fd = thread_index + 3;
    if (::strcmp(protocol, "tcp") == 0)
        p->use_header = 1;
    else
        p->use_header = 0;
    p->buffer_size = buffer_size;
    p->lookup = new lookup_t();
    p->buffer_send = new buffer_t(32768, CLOUDI_MAX_BUFFERSIZE);
    p->buffer_recv = new buffer_t(32768, CLOUDI_MAX_BUFFERSIZE);
    p->buffer_recv_index = 0;
    p->buffer_call = new buffer_t(32768, CLOUDI_MAX_BUFFERSIZE);
    p->prefix = 0;

    ::atexit(&exit_handler);

    // attempt initialization
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index;
    if (p->use_header)
        index = 4;
    else
        index = 0;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, "init"))
        return cloudi_error_ei_encode;
    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;

    while (cloudi_timeout == (result = cloudi_poll(p, 1000)));

    return result;
}

void cloudi_destroy(cloudi_instance_t * p)
{
    if (p->fd != 0)
    {
        ::close(p->fd);
        delete reinterpret_cast<lookup_t *>(p->lookup);
        delete reinterpret_cast<buffer_t *>(p->buffer_send);
        delete reinterpret_cast<buffer_t *>(p->buffer_recv);
        delete reinterpret_cast<buffer_t *>(p->buffer_call);
        if (p->prefix)
            delete p->prefix;
    }
}

int cloudi_initialize_thread_count(unsigned int * const thread_count)
{
    char const * const p = ::getenv("CLOUDI_API_INIT_THREAD_COUNT");
    if (p == 0)
        return cloudi_invalid_input;
    int const value = ::atoi(p);
    if (value < 0)
        return cloudi_invalid_input;
    *thread_count = static_cast<unsigned int>(value);
    return cloudi_success;
}

static int cloudi_subscribe_(cloudi_instance_t * p,
                             char const * const pattern,
                             callback_function const & f)
{
    lookup_t & lookup = *reinterpret_cast<lookup_t *>(p->lookup);
    lookup.insert(std::string(p->prefix) + pattern, f);

    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_tuple_header(buffer.get<char>(), &index, 2))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, "subscribe"))
        return cloudi_error_ei_encode;
    if (buffer.reserve(index + strlen(pattern) + 1) == false)
        return cloudi_error_write_overflow;
    if (ei_encode_string(buffer.get<char>(), &index, pattern))
        return cloudi_error_ei_encode;
    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    return cloudi_success;
}

int cloudi_subscribe(cloudi_instance_t * p,
                     char const * const pattern,
                     cloudi_callback_t f)
{
    return cloudi_subscribe_(p,
                             pattern,
                             callback_function(p, f));
}

int cloudi_unsubscribe(cloudi_instance_t * p,
                       char const * const pattern)
{
    std::string str(p->prefix);
    str += pattern;
    lookup_t & lookup = *reinterpret_cast<lookup_t *>(p->lookup);
    if (lookup.erase(str))
    {
        buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
        int index = 0;
        if (p->use_header)
            index = 4;
        if (ei_encode_version(buffer.get<char>(), &index))
            return cloudi_error_ei_encode;
        if (ei_encode_tuple_header(buffer.get<char>(), &index, 2))
            return cloudi_error_ei_encode;
        if (ei_encode_atom(buffer.get<char>(), &index, "unsubscribe"))
            return cloudi_error_ei_encode;
        if (buffer.reserve(index + strlen(pattern) + 1) == false)
            return cloudi_error_write_overflow;
        if (ei_encode_string(buffer.get<char>(), &index, pattern))
            return cloudi_error_ei_encode;
        int result = write_exact(p->fd, p->use_header,
                                 buffer.get<char>(), index);
        if (result)
            return result;
        return cloudi_success;
    }
    else
    {
        return cloudi_error_function_parameter;
    }
}

static int cloudi_send_(cloudi_instance_t * p,
                        char const * const command_name,
                        char const * const name,
                        void const * const request_info,
                        uint32_t const request_info_size,
                        void const * const request,
                        uint32_t const request_size,
                        uint32_t timeout,
                        int8_t const priority)
{
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_tuple_header(buffer.get<char>(), &index, 6))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, command_name))
        return cloudi_error_ei_encode;
    if (buffer.reserve(index + strlen(name) + 1 +
                       request_info_size + request_size) == false)
        return cloudi_error_write_overflow;
    if (ei_encode_string(buffer.get<char>(), &index, name))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index,
                         request_info, request_info_size))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index, request, request_size))
        return cloudi_error_ei_encode;
    if (ei_encode_ulong(buffer.get<char>(), &index, timeout))
        return cloudi_error_ei_encode;
    if (ei_encode_long(buffer.get<char>(), &index, priority))
        return cloudi_error_ei_encode;
    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    result = cloudi_poll(p, -1);
    if (result)
        return result;
    return cloudi_success;
}

int cloudi_send_async(cloudi_instance_t * p,
                      char const * const name,
                      void const * const request,
                      uint32_t const request_size)
{
    return cloudi_send_(p, "send_async", name, "", 0,
                        request, request_size,
                        p->timeout_async, p->priority_default);
}

int cloudi_send_async_(cloudi_instance_t * p,
                       char const * const name,
                       void const * const request_info,
                       uint32_t const request_info_size,
                       void const * const request,
                       uint32_t const request_size,
                       uint32_t timeout,
                       int8_t const priority)
{
    if (timeout == 0)
        timeout = p->timeout_async;
    return cloudi_send_(p, "send_async", name,
                        request_info, request_info_size,
                        request, request_size, timeout, priority);
}

int cloudi_send_sync(cloudi_instance_t * p,
                     char const * const name,
                     void const * const request,
                     uint32_t const request_size)
{
    return cloudi_send_(p, "send_sync", name, "", 0,
                        request, request_size,
                        p->timeout_sync, p->priority_default);
}

int cloudi_send_sync_(cloudi_instance_t * p,
                      char const * const name,
                      void const * const request_info,
                      uint32_t const request_info_size,
                      void const * const request,
                      uint32_t const request_size,
                      uint32_t timeout,
                      int8_t const priority)
{
    if (timeout == 0)
        timeout = p->timeout_sync;
    return cloudi_send_(p, "send_sync", name,
                        request_info, request_info_size,
                        request, request_size, timeout, priority);
}

int cloudi_mcast_async(cloudi_instance_t * p,
                       char const * const name,
                       void const * const request,
                       uint32_t const request_size)
{
    return cloudi_send_(p, "mcast_async", name, "", 0,
                        request, request_size,
                        p->timeout_async, p->priority_default);
}

int cloudi_mcast_async_(cloudi_instance_t * p,
                        char const * const name,
                        void const * const request_info,
                        uint32_t const request_info_size,
                        void const * const request,
                        uint32_t const request_size,
                        uint32_t timeout,
                        int8_t const priority)
{
    if (timeout == 0)
        timeout = p->timeout_async;
    return cloudi_send_(p, "mcast_async", name,
                        request_info, request_info_size,
                        request, request_size, timeout, priority);
}

static int cloudi_forward_(cloudi_instance_t * p,
                           char const * const command_name,
                           char const * const name,
                           void const * const request_info,
                           uint32_t const request_info_size,
                           void const * const request,
                           uint32_t const request_size,
                           uint32_t timeout,
                           int8_t const priority,
                           char const * const trans_id,
                           char const * const pid,
                           uint32_t const pid_size)
{
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_tuple_header(buffer.get<char>(), &index, 8))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, command_name))
        return cloudi_error_ei_encode;
    if (buffer.reserve(index + strlen(name) + 1 +
                       request_info_size + request_size + pid_size) == false)
        return cloudi_error_write_overflow;
    if (ei_encode_string(buffer.get<char>(), &index, name))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index,
                         request_info, request_info_size))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index, request, request_size))
        return cloudi_error_ei_encode;
    if (ei_encode_ulong(buffer.get<char>(), &index, timeout))
        return cloudi_error_ei_encode;
    if (ei_encode_long(buffer.get<char>(), &index, priority))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index, trans_id, 16))
        return cloudi_error_ei_encode;
    int version;
    int pid_index = 0;
    if (ei_decode_version(pid, &pid_index, &version))
        return cloudi_error_ei_decode;
    int const pid_data_size = pid_size - pid_index;
    ::memcpy(&(buffer[index]), &(pid[pid_index]), pid_data_size);
    index += pid_data_size;

    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    return cloudi_success;
}

int cloudi_forward(cloudi_instance_t * p,
                   int const command,
                   char const * const name,
                   void const * const request_info,
                   uint32_t const request_info_size,
                   void const * const request,
                   uint32_t const request_size,
                   uint32_t timeout,
                   int8_t const priority,
                   char const * const trans_id,
                   char const * const pid,
                   uint32_t const pid_size)
{
    int result;
    if (command > 0)   // CLOUDI_ASYNC
    {
        result = cloudi_forward_(p, "forward_async", name,
                                 request_info, request_info_size,
                                 request, request_size,
                                 timeout, priority, trans_id, pid, pid_size);
        assert(result == cloudi_success);
        throw CloudI::API::return_async_exception();
    }
    else               // CLOUDI_SYNC
    {
        result = cloudi_forward_(p, "forward_sync", name,
                                 request_info, request_info_size,
                                 request, request_size,
                                 timeout, priority, trans_id, pid, pid_size);
        assert(result == cloudi_success);
        throw CloudI::API::return_sync_exception();
    }
    return result;
}

int cloudi_forward_async(cloudi_instance_t * p,
                         char const * const name,
                         void const * const request_info,
                         uint32_t const request_info_size,
                         void const * const request,
                         uint32_t const request_size,
                         uint32_t timeout,
                         int8_t const priority,
                         char const * const trans_id,
                         char const * const pid,
                         uint32_t const pid_size)
{
    int const result = cloudi_forward_(p, "forward_async", name,
                                       request_info, request_info_size,
                                       request, request_size,
                                       timeout, priority,
                                       trans_id, pid, pid_size);
    assert(result == cloudi_success);
    throw CloudI::API::return_async_exception();
    return result;
}

int cloudi_forward_sync(cloudi_instance_t * p,
                        char const * const name,
                        void const * const request_info,
                        uint32_t const request_info_size,
                        void const * const request,
                        uint32_t const request_size,
                        uint32_t timeout,
                        int8_t const priority,
                        char const * const trans_id,
                        char const * const pid,
                        uint32_t const pid_size)
{
    int const result = cloudi_forward_(p, "forward_sync", name,
                                       request_info, request_info_size,
                                       request, request_size,
                                       timeout, priority,
                                       trans_id, pid, pid_size);
    assert(result == cloudi_success);
    throw CloudI::API::return_sync_exception();
    return result;
}

static int cloudi_return_(cloudi_instance_t * p,
                          char const * const command_name,
                          char const * const name,
                          char const * const pattern,
                          void const * const response_info,
                          uint32_t const response_info_size,
                          void const * const response,
                          uint32_t const response_size,
                          uint32_t timeout,
                          char const * const trans_id,
                          char const * const pid,
                          uint32_t const pid_size)
{
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_tuple_header(buffer.get<char>(), &index, 8))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, command_name))
        return cloudi_error_ei_encode;
    if (buffer.reserve(index + strlen(name) + 1 + strlen(pattern) + 1 +
                       response_info_size + response_size + pid_size) == false)
        return cloudi_error_write_overflow;
    if (ei_encode_string(buffer.get<char>(), &index, name))
        return cloudi_error_ei_encode;
    if (ei_encode_string(buffer.get<char>(), &index, pattern))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index,
                         response_info, response_info_size))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index, response, response_size))
        return cloudi_error_ei_encode;
    if (ei_encode_ulong(buffer.get<char>(), &index, timeout))
        return cloudi_error_ei_encode;
    if (ei_encode_binary(buffer.get<char>(), &index, trans_id, 16))
        return cloudi_error_ei_encode;
    int version;
    int pid_index = 0;
    if (ei_decode_version(pid, &pid_index, &version))
        return cloudi_error_ei_decode;
    int const pid_data_size = pid_size - pid_index;
    ::memcpy(&(buffer[index]), &(pid[pid_index]), pid_data_size);
    index += pid_data_size;

    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    return cloudi_success;
}

int cloudi_return(cloudi_instance_t * p,
                  int const command,
                  char const * const name,
                  char const * const pattern,
                  void const * const response_info,
                  uint32_t const response_info_size,
                  void const * const response,
                  uint32_t const response_size,
                  uint32_t timeout,
                  char const * const trans_id,
                  char const * const pid,
                  uint32_t const pid_size)
{
    int result;
    if (command > 0)   // CLOUDI_ASYNC
    {
        result = cloudi_return_(p, "return_async", name, pattern,
                                response_info, response_info_size,
                                response, response_size,
                                timeout, trans_id, pid, pid_size);
        if (result != cloudi_success)
            std::cerr << "return_async failed: " << result << std::endl;
        throw CloudI::API::return_async_exception();
    }
    else               // CLOUDI_SYNC
    {
        result = cloudi_return_(p, "return_sync", name, pattern,
                                response_info, response_info_size,
                                response, response_size,
                                timeout, trans_id, pid, pid_size);
        if (result != cloudi_success)
            std::cerr << "return_sync failed: " << result << std::endl;
        throw CloudI::API::return_sync_exception();
    }
    return result;
}

int cloudi_return_async(cloudi_instance_t * p,
                        char const * const name,
                        char const * const pattern,
                        void const * const response_info,
                        uint32_t const response_info_size,
                        void const * const response,
                        uint32_t const response_size,
                        uint32_t timeout,
                        char const * const trans_id,
                        char const * const pid,
                        uint32_t const pid_size)
{
    int const result = cloudi_return_(p, "return_async", name, pattern,
                                      response_info, response_info_size,
                                      response, response_size,
                                      timeout, trans_id, pid, pid_size);
    assert(result == cloudi_success);
    throw CloudI::API::return_async_exception();
    return result;
}

int cloudi_return_sync(cloudi_instance_t * p,
                       char const * const name,
                       char const * const pattern,
                       void const * const response_info,
                       uint32_t const response_info_size,
                       void const * const response,
                       uint32_t const response_size,
                       uint32_t timeout,
                       char const * const trans_id,
                       char const * const pid,
                       uint32_t const pid_size)
{
    int const result = cloudi_return_(p, "return_sync", name, pattern,
                                      response_info, response_info_size,
                                      response, response_size,
                                      timeout, trans_id, pid, pid_size);
    assert(result == cloudi_success);
    throw CloudI::API::return_sync_exception();
    return result;
}

int cloudi_recv_async(cloudi_instance_t * p,
                      uint32_t timeout,
                      char const * const trans_id)
{
    char const trans_id_null[16] = {0, 0, 0, 0, 0, 0, 0, 0, 
                                    0, 0, 0, 0, 0, 0, 0, 0};
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
        
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_tuple_header(buffer.get<char>(), &index, 3))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, "recv_async"))
        return cloudi_error_ei_encode;
    if (timeout == 0)
        timeout = p->timeout_sync;
    if (ei_encode_ulong(buffer.get<char>(), &index, timeout))
        return cloudi_error_ei_encode;
    if (trans_id == 0)
    {
        if (ei_encode_binary(buffer.get<char>(), &index, trans_id_null, 16))
            return cloudi_error_ei_encode;
    }
    else
    {
        if (ei_encode_binary(buffer.get<char>(), &index, trans_id, 16))
            return cloudi_error_ei_encode;
    }
    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    result = cloudi_poll(p, -1);
    if (result)
        return result;
    return cloudi_success;
}

static int keepalive(cloudi_instance_t * p)
{
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_send);
    int index = 0;
    if (p->use_header)
        index = 4;
    if (ei_encode_version(buffer.get<char>(), &index))
        return cloudi_error_ei_encode;
    if (ei_encode_atom(buffer.get<char>(), &index, "keepalive"))
        return cloudi_error_ei_encode;
    int result = write_exact(p->fd, p->use_header, buffer.get<char>(), index);
    if (result)
        return result;
    return cloudi_success;
}

#define MESSAGE_INIT           1
#define MESSAGE_SEND_ASYNC     2
#define MESSAGE_SEND_SYNC      3
#define MESSAGE_RECV_ASYNC     4
#define MESSAGE_RETURN_ASYNC   5
#define MESSAGE_RETURN_SYNC    6
#define MESSAGE_RETURNS_ASYNC  7
#define MESSAGE_KEEPALIVE      8

static void callback(cloudi_instance_t * p,
                     int const command,
                     char const * const name,
                     char const * const pattern,
                     void const * const request_info,
                     uint32_t const request_info_size,
                     void const * const request,
                     uint32_t const request_size,
                     uint32_t timeout,
                     int8_t const priority,
                     char const * const trans_id,
                     char const * const pid,
                     uint32_t const pid_size)
{
    lookup_t & lookup = *reinterpret_cast<lookup_t *>(p->lookup);
    callback_function f = lookup.find(std::string(pattern));
    
    if (command == MESSAGE_SEND_ASYNC)
    {
        try
        {
            f(CLOUDI_ASYNC, name, pattern,
              request_info, request_info_size,
              request, request_size,
              timeout, priority, trans_id, pid, pid_size);
        }
        catch (CloudI::API::return_async_exception const &)
        {
            return;
        }
        catch (CloudI::API::return_sync_exception const &)
        {
            assert(false);
            return;
        }
        catch (std::exception const & e)
        {
            std::cerr << "exception: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "exception: (unknown)" << std::endl;
        }
        cloudi_return_(p, "return_async", name, pattern, "", 0, "", 0,
                       timeout, trans_id, pid, pid_size);
    }
    else if (command == MESSAGE_SEND_SYNC)
    {
        try
        {
            f(CLOUDI_SYNC, name, pattern,
              request_info, request_info_size,
              request, request_size,
              timeout, priority, trans_id, pid, pid_size);
        }
        catch (CloudI::API::return_async_exception const &)
        {
            assert(false);
            return;
        }
        catch (CloudI::API::return_sync_exception const &)
        {
            return;
        }
        catch (std::exception const & e)
        {
            std::cerr << "exception: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "exception: (unknown)" << std::endl;
        }
        cloudi_return_(p, "return_sync", name, pattern, "", 0, "", 0,
                       timeout, trans_id, pid, pid_size);
    }
    else
    {
        assert(false);
    }
}

static void store_incoming_binary(buffer_t const & buffer,
                                  uint32_t & index,
                                  char * & p)
{
    uint32_t size = *reinterpret_cast<uint32_t *>(&buffer[index]);
    index += sizeof(uint32_t);
    p = new char[size];
    memcpy(p, &buffer[index], size);
    index += size;
}

static void store_incoming_uint32(buffer_t const & buffer,
                                  uint32_t & index,
                                  uint32_t & i)
{
    i = *reinterpret_cast<uint32_t *>(&buffer[index]);
    index += sizeof(uint32_t);
}

static void store_incoming_int8(buffer_t const & buffer,
                                uint32_t & index,
                                int8_t & i)
{
    i = *reinterpret_cast<int8_t *>(&buffer[index]);
    index += sizeof(int8_t);
}

int cloudi_poll(cloudi_instance_t * p,
                int timeout)
{
    buffer_t & buffer = *reinterpret_cast<buffer_t *>(p->buffer_recv);
    buffer_t & buffer_call = *reinterpret_cast<buffer_t *>(p->buffer_call);
    struct pollfd fds[1] = {{p->fd, POLLIN | POLLPRI, 0}};
    int count = ::poll(fds, 1, timeout);
    if (count == 0)
        return cloudi_timeout;
    else if (count < 0)
        return errno_poll();

    int result = read_all(p->fd, p->use_header,
                          buffer, p->buffer_recv_index,
                          p->buffer_size);
    if (result)
        return result;
        
    while (true)
    {
        if (p->buffer_recv_index == 0)
            ::exit(cloudi_error_read_underflow);

        fds[0].revents = 0;
        uint32_t index = 0;
        uint32_t command;
        store_incoming_uint32(buffer, index, command);
        switch (command)
        {
            case MESSAGE_INIT:
            {
                store_incoming_binary(buffer, index, p->prefix);
                store_incoming_uint32(buffer, index, p->timeout_async);
                store_incoming_uint32(buffer, index, p->timeout_sync);
                store_incoming_int8(buffer, index, p->priority_default);
                if (index != p->buffer_recv_index)
                    ::exit(cloudi_error_read_underflow);
                p->buffer_recv_index = 0;
                return cloudi_success;
            }
            case MESSAGE_SEND_ASYNC:
            case MESSAGE_SEND_SYNC:
            {
                buffer_call.copy(buffer);
                uint32_t name_size;
                store_incoming_uint32(buffer_call, index, name_size);
                char * name = &buffer_call[index];
                index += name_size;
                uint32_t pattern_size;
                store_incoming_uint32(buffer_call, index, pattern_size);
                char * pattern = &buffer_call[index];
                index += pattern_size;
                uint32_t request_info_size;
                store_incoming_uint32(buffer_call, index, request_info_size);
                char * request_info = &buffer_call[index];
                index += request_info_size + 1;
                uint32_t request_size;
                store_incoming_uint32(buffer_call, index, request_size);
                char * request = &buffer_call[index];
                index += request_size + 1;
                uint32_t timeout;
                store_incoming_uint32(buffer_call, index, timeout);
                int8_t priority;
                store_incoming_int8(buffer_call, index, priority);
                char * trans_id = &buffer_call[index];
                index += 16;
                uint32_t pid_size;
                store_incoming_uint32(buffer_call, index, pid_size);
                char * pid = &buffer_call[index];
                index += pid_size;
                if (index != p->buffer_recv_index)
                    return cloudi_error_read_underflow;
                p->buffer_recv_index = 0;
                callback(p, command, name, pattern,
                         request_info, request_info_size,
                         request, request_size,
                         timeout, priority, trans_id, pid, pid_size);
                break;
            }
            case MESSAGE_RECV_ASYNC:
            case MESSAGE_RETURN_SYNC:
            {
                store_incoming_uint32(buffer, index, p->response_info_size);
                p->response_info = &buffer[index];
                index += p->response_info_size + 1;
                store_incoming_uint32(buffer, index, p->response_size);
                p->response = &buffer[index];
                index += p->response_size + 1;
                p->trans_id_count = 1;
                p->trans_id = &buffer[index];
                index += 16;
                if (index != p->buffer_recv_index)
                    ::exit(cloudi_error_read_underflow);
                p->buffer_recv_index = 0;
                return cloudi_success;
            }
            case MESSAGE_RETURN_ASYNC:
            {
                p->trans_id_count = 1;
                p->trans_id = &buffer[index];
                index += 16;
                if (index != p->buffer_recv_index)
                    ::exit(cloudi_error_read_underflow);
                p->buffer_recv_index = 0;
                return cloudi_success;
            }
            case MESSAGE_RETURNS_ASYNC:
            {
                store_incoming_uint32(buffer, index, p->trans_id_count);
                p->trans_id = &buffer[index];
                index += 16 * p->trans_id_count;
                if (index != p->buffer_recv_index)
                    ::exit(cloudi_error_read_underflow);
                p->buffer_recv_index = 0;
                return cloudi_success;
            }
            case MESSAGE_KEEPALIVE:
            {
                if (index > p->buffer_recv_index)
                    ::exit(cloudi_error_read_underflow);
                result = keepalive(p);
                if (result)
                    ::exit(result);
                if (index < p->buffer_recv_index) {
                    p->buffer_recv_index -= index;
                    buffer.move(index, p->buffer_recv_index, 0);
                    assert(p->use_header == false);
                    count = ::poll(fds, 1, 0);
                    if (count < 0)
                        return errno_poll();
                    else if (count == 0)
                        continue;
                }
                else {
                    p->buffer_recv_index = 0;
                }
                break;
            }
            default:
            {
                ::exit(cloudi_error_read_underflow);
            }
        }

        fds[0].revents = 0;
        count = ::poll(fds, 1, timeout);
        if (count == 0)
            return cloudi_timeout;
        else if (count < 0)
            return errno_poll();

        result = read_all(p->fd, p->use_header,
                          buffer, p->buffer_recv_index,
                          p->buffer_size);
        if (result)
            return result;
    }
}

static char const ** binary_key_value_parse(void const * const binary,
                                            uint32_t const binary_size)
{
    char const * p = reinterpret_cast<char const * const>(binary);
    realloc_ptr<char const *> result(16, 8192);
    result[0] = p;
    size_t i = 1;
    for (size_t binary_i = 1; binary_i < binary_size; ++binary_i)
    {
        if (p[binary_i] == '\0')
        {
            result[i] = &p[++binary_i];
            result.reserve(++i + 1);
        }
    }
    result[i] = 0;
    return result.release();
}

static void binary_key_value_destroy(char const ** p)
{
    free(p);
}

// CloudI helper functions

char const ** cloudi_request_http_qs_parse(void const * const request,
                                           uint32_t const request_size)
{
    return binary_key_value_parse(request, request_size);
}

void cloudi_request_http_qs_destroy(char const ** p)
{
    binary_key_value_destroy(p);
}

char const ** cloudi_info_key_value_parse(void const * const message_info,
                                          uint32_t const message_info_size)
{
    return binary_key_value_parse(message_info, message_info_size);
}

void cloudi_info_key_value_destroy(char const ** p)
{
    binary_key_value_destroy(p);
}

} // extern C

// C++ API
namespace CloudI
{

API::API(unsigned int const thread_index) :
    m_api(new cloudi_instance_t()),
    m_count(new int)
{
    (*m_count) = 1;
    int const result = cloudi_initialize(m_api, thread_index);
    if (result != return_value::success)
        throw invalid_input_exception();
}

API::~API()
{
    if (--(*m_count) == 0)
    {
        cloudi_destroy(m_api);
        delete m_api;
        delete m_count;
    }
}

API::API(API const & object) :
    m_api(object.m_api),
    m_count(object.m_count)
{
    ++(*m_count);
}

unsigned int API::thread_count()
{
    unsigned int thread_count;
    int const result = cloudi_initialize_thread_count(&thread_count);
    if (result != return_value::success)
        throw invalid_input_exception();
    return thread_count;
}

int API::subscribe(char const * const pattern,
                   API::callback_function_generic * p) const
{
    return cloudi_subscribe_(m_api,
                             pattern,
                             callback_function(p));
}

int API::unsubscribe(char const * const pattern) const
{
    return cloudi_unsubscribe(m_api,
                              pattern);
}

int API::send_async(char const * const name,
                    void const * const request,
                    uint32_t const request_size) const
{
    return cloudi_send_async(m_api,
                             name,
                             request,
                             request_size);
}

int API::send_async(char const * const name,
                    void const * const request_info,
                    uint32_t const request_info_size,
                    void const * const request,
                    uint32_t const request_size,
                    uint32_t timeout,
                    int8_t const priority) const
{
    return cloudi_send_async_(m_api,
                             name,
                             request_info,
                             request_info_size,
                             request,
                             request_size,
                             timeout,
                             priority);
}

int API::send_sync(char const * const name,
                   void const * const request,
                   uint32_t const request_size) const
{
    return cloudi_send_sync(m_api,
                            name,
                            request,
                            request_size);
}

int API::send_sync(char const * const name,
                   void const * const request_info,
                   uint32_t const request_info_size,
                   void const * const request,
                   uint32_t const request_size,
                   uint32_t timeout,
                   int8_t const priority) const
{
    return cloudi_send_sync_(m_api,
                             name,
                             request_info,
                             request_info_size,
                             request,
                             request_size,
                             timeout,
                             priority);
}

int API::mcast_async(char const * const name,
                     void const * const request,
                     uint32_t const request_size) const
{
    return cloudi_mcast_async(m_api,
                              name,
                              request,
                              request_size);
}

int API::mcast_async(char const * const name,
                     void const * const request_info,
                     uint32_t const request_info_size,
                     void const * const request,
                     uint32_t const request_size,
                     uint32_t timeout,
                     int8_t const priority) const
{
    return cloudi_mcast_async_(m_api,
                               name,
                               request_info,
                               request_info_size,
                               request,
                               request_size,
                               timeout,
                               priority);
}

char const * API::get_response() const
{
    return m_api->response;
}

uint32_t API::get_response_size() const
{
    return m_api->response_size;
}

char const * API::get_response_info() const
{
    return m_api->response_info;
}

uint32_t API::get_response_info_size() const
{
    return m_api->response_info_size;
}

uint32_t API::get_trans_id_count() const
{
    return m_api->trans_id_count;
}

char const * API::get_trans_id(unsigned int const i) const
{
    if (i >= m_api->trans_id_count)
        return 0;
    return &(m_api->trans_id[i * 16]);
}

int API::forward_(int const command,
                  char const * const name,
                  void const * const request_info,
                  uint32_t const request_info_size,
                  void const * const request,
                  uint32_t const request_size,
                  uint32_t timeout,
                  int8_t const priority,
                  char const * const trans_id,
                  char const * const pid,
                  uint32_t const pid_size) const
{
    return cloudi_forward(m_api,
                          command,
                          name,
                          request_info,
                          request_info_size,
                          request,
                          request_size,
                          timeout,
                          priority,
                          trans_id,
                          pid,
                          pid_size);
}

int API::forward_async(char const * const name,
                       void const * const request_info,
                       uint32_t const request_info_size,
                       void const * const request,
                       uint32_t const request_size,
                       uint32_t timeout,
                       int8_t const priority,
                       char const * const trans_id,
                       char const * const pid,
                       uint32_t const pid_size) const
{
    return cloudi_forward_async(m_api,
                                name,
                                request_info,
                                request_info_size,
                                request,
                                request_size,
                                timeout,
                                priority,
                                trans_id,
                                pid,
                                pid_size);
}

int API::forward_sync(char const * const name,
                      void const * const request_info,
                      uint32_t const request_info_size,
                      void const * const request,
                      uint32_t const request_size,
                      uint32_t timeout,
                      int8_t const priority,
                      char const * const trans_id,
                      char const * const pid,
                      uint32_t const pid_size) const
{
    return cloudi_forward_sync(m_api,
                               name,
                               request_info,
                               request_info_size,
                               request,
                               request_size,
                               timeout,
                               priority,
                               trans_id,
                               pid,
                               pid_size);
}

int API::return_(int const command,
                 char const * const name,
                 char const * const pattern,
                 void const * const response_info,
                 uint32_t const response_info_size,
                 void const * const response,
                 uint32_t const response_size,
                 uint32_t timeout,
                 char const * const trans_id,
                 char const * const pid,
                 uint32_t const pid_size) const
{
    return cloudi_return(m_api,
                         command,
                         name,
                         pattern,
                         response_info,
                         response_info_size,
                         response,
                         response_size,
                         timeout,
                         trans_id,
                         pid,
                         pid_size);
}

int API::return_async(char const * const name,
                      char const * const pattern,
                      void const * const response_info,
                      uint32_t const response_info_size,
                      void const * const response,
                      uint32_t const response_size,
                      uint32_t timeout,
                      char const * const trans_id,
                      char const * const pid,
                      uint32_t const pid_size) const
{
    return cloudi_return_async(m_api,
                               name,
                               pattern,
                               response_info,
                               response_info_size,
                               response,
                               response_size,
                               timeout,
                               trans_id,
                               pid,
                               pid_size);
}

int API::return_sync(char const * const name,
                     char const * const pattern,
                     void const * const response_info,
                     uint32_t const response_info_size,
                     void const * const response,
                     uint32_t const response_size,
                     uint32_t timeout,
                     char const * const trans_id,
                     char const * const pid,
                     uint32_t const pid_size) const
{
    return cloudi_return_sync(m_api,
                              name,
                              pattern,
                              response_info,
                              response_info_size,
                              response,
                              response_size,
                              timeout,
                              trans_id,
                              pid,
                              pid_size);
}

int API::recv_async() const
{
    return cloudi_recv_async(m_api,
                             m_api->timeout_sync,
                             0);
}

int API::recv_async(char const * const trans_id) const
{
    return cloudi_recv_async(m_api,
                             m_api->timeout_sync,
                             trans_id);
}

int API::recv_async(uint32_t timeout,
                    char const * const trans_id) const
{
    return cloudi_recv_async(m_api,
                             timeout,
                             trans_id);
}

char const * API::prefix() const
{
    return m_api->prefix;
}

uint32_t API::timeout_async() const
{
    return m_api->timeout_async;
}

uint32_t API::timeout_sync() const
{
    return m_api->timeout_sync;
}

int API::poll(int timeout) const
{
    return cloudi_poll(m_api,
                       timeout);
}

char const ** API::request_http_qs_parse(void const * const request,
                                         uint32_t const request_size) const
{
    return cloudi_request_http_qs_parse(request,
                                        request_size);
}

void API::request_http_qs_destroy(char const ** p) const
{
    cloudi_request_http_qs_destroy(p);
}

char const ** API::info_key_value_parse(void const * const message_info,
                                        uint32_t const message_info_size) const
{
    return cloudi_info_key_value_parse(message_info,
                                       message_info_size);
}

void API::info_key_value_destroy(char const ** p) const
{
    cloudi_info_key_value_destroy(p);
}

} // namespace CloudI

