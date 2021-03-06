#!/usr/bin/env ruby
# -*- coding: utf-8; Mode: ruby; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*-
# ex: set softtabstop=4 tabstop=4 shiftwidth=4 expandtab fileencoding=utf-8:
#
# BSD LICENSE
# 
# Copyright (c) 2011-2012, Michael Truog <mjtruog at gmail dot com>
# All rights reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#     * All advertising materials mentioning features or use of this
#       software must display the following acknowledgment:
#         This product includes software developed by Michael Truog
#     * The name of the author may not be used to endorse or promote
#       products derived from this software without specific prior
#       written permission
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
# CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
# INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.
#

$:.unshift File.dirname(__FILE__)

$stdout.sync = true
$stderr.sync = true

require 'erlang'

module CloudI
    class API
        include Erlang
        # unbuffered output is with $stderr.puts '...'

        ASYNC  =  1
        SYNC   = -1

        def initialize(thread_index)
            protocol = API.getenv('CLOUDI_API_INIT_PROTOCOL')
            buffer_size_str = API.getenv('CLOUDI_API_INIT_BUFFER_SIZE')
            @socket = IO.for_fd(thread_index + 3, File::RDWR, autoclose: false)
            @socket.sync = true
            @use_header = (protocol == 'tcp')
            @size = buffer_size_str.to_i
            @callbacks = Hash.new
            send(term_to_binary(:init))
            poll
        end

        def self.thread_count
            s = getenv('CLOUDI_API_INIT_THREAD_COUNT')
            s.to_i
        end

        def subscribe(pattern, function)
            key = @prefix + pattern
            value = @callbacks.fetch(key, nil)
            if value.nil?
                @callbacks[key] = [function]
            else
                value.push(function)
            end
            send(term_to_binary([:subscribe, pattern]))
        end

        def unsubscribe(pattern)
            @callbacks.delete(@prefix + pattern)
            send(term_to_binary([:unsubscribe, pattern]))
        end

        def send_async(name, request,
                       timeout=nil, request_info=nil, priority=nil)
            if timeout.nil?
                timeout = @timeoutAsync
            end
            if request_info.nil?
                request_info = ''
            end
            if priority.nil?
                priority = @priorityDefault
            end
            send(term_to_binary([:send_async, name,
                                  OtpErlangBinary.new(request_info),
                                  OtpErlangBinary.new(request),
                                  timeout, priority]))
            return poll
        end

        def send_sync(name, request,
                      timeout=nil, request_info=nil, priority=nil)
            if timeout.nil?
                timeout = @timeoutSync
            end
            if request_info.nil?
                request_info = ''
            end
            if priority.nil?
                priority = @priorityDefault
            end
            send(term_to_binary([:send_sync, name,
                                  OtpErlangBinary.new(request_info),
                                  OtpErlangBinary.new(request),
                                  timeout, priority]))
            return poll
        end

        def mcast_async(name, request,
                        timeout=nil, request_info=nil, priority=nil)
            if timeout.nil?
                timeout = @timeoutAsync
            end
            if request_info.nil?
                request_info = ''
            end
            if priority.nil?
                priority = @priorityDefault
            end
            send(term_to_binary([:mcast_async, name,
                                  OtpErlangBinary.new(request_info),
                                  OtpErlangBinary.new(request),
                                  timeout, priority]))
            return poll
        end

        def forward_(command, name, request_info, request,
                     timeout, priority, transId, pid)
            case command
            when ASYNC
                forward_async(name, request_info, request,
                              timeout, priority, transId, pid)
            when SYNC
                forward_sync(name, request_info, request,
                             timeout, priority, transId, pid)
            end
        end

        def forward_async(name, request_info, request,
                          timeout, priority, transId, pid)
            send(term_to_binary([:forward_async, name,
                                  OtpErlangBinary.new(request_info),
                                  OtpErlangBinary.new(request),
                                  timeout, priority,
                                  OtpErlangBinary.new(transId), pid]))
            raise ReturnAsyncException
        end

        def forward_sync(name, request_info, request,
                         timeout, priority, transId, pid)
            send(term_to_binary([:forward_sync, name, 
                                  OtpErlangBinary.new(request_info),
                                  OtpErlangBinary.new(request),
                                  timeout, priority,
                                  OtpErlangBinary.new(transId), pid]))
            raise ReturnSyncException
        end

        def return_(command, name, pattern, response_info, response,
                    timeout, transId, pid)
            case command
            when ASYNC
                return_async(name, pattern, response_info, response,
                             timeout, transId, pid)
            when SYNC
                return_sync(name, pattern, response_info, response,
                            timeout, transId, pid)
            end
        end

        def return_async(name, pattern, response_info, response,
                         timeout, transId, pid)
            return_async_nothrow(name, pattern, response_info, response,
                                 timeout, transId, pid)
            raise ReturnAsyncException
        end

        def return_async_nothrow(name, pattern, response_info, response,
                                 timeout, transId, pid)
            send(term_to_binary([:return_async, name, pattern,
                                  OtpErlangBinary.new(response_info),
                                  OtpErlangBinary.new(response),
                                  timeout,
                                  OtpErlangBinary.new(transId), pid]))
        end

        def return_sync(name, pattern, response_info, response,
                        timeout, transId, pid)
            return_sync_nothrow(name, pattern, response_info, response,
                                timeout, transId, pid)
            raise ReturnSyncException
        end

        def return_sync_nothrow(name, pattern, response_info, response,
                                timeout, transId, pid)
            send(term_to_binary([:return_sync, name, pattern,
                                  OtpErlangBinary.new(response_info),
                                  OtpErlangBinary.new(response),
                                  timeout,
                                  OtpErlangBinary.new(transId), pid]))
        end

        def recv_async(timeout=nil, transId=nil)
            if timeout.nil?
                timeout = @timeoutSync
            end
            if transId.nil?
                transId = 0.chr * 16
            end
            send(term_to_binary([:recv_async, timeout,
                                  OtpErlangBinary.new(transId)]))
            return poll
        end

        def prefix
            return @prefix
        end

        def timeout_async
            return @timeoutAsync
        end

        def timeout_sync
            return @timeoutSync
        end

        def callback(command, name, pattern, requestInfo, request,
                     timeout, priority, transId, pid)
            function_queue = @callbacks.fetch(pattern, nil)
            API.assert{function_queue != nil}
            function = function_queue.shift
            function_queue.push(function)
            case command
            when MESSAGE_SEND_ASYNC
                begin
                    response = function.call(ASYNC, name, pattern,
                                             requestInfo, request,
                                             timeout, priority, transId, pid)
                    if response.kind_of?(Array)
                        API.assert{response.length == 2}
                        responseInfo = response[0]
                        response = response[1]
                        if not responseInfo.kind_of?(String)
                            responseInfo = ''
                        end
                    else
                        responseInfo = ''
                    end
                    if not response.kind_of?(String)
                        response = ''
                    end
                rescue ReturnAsyncException
                    return
                rescue ReturnSyncException => e
                    $stderr.puts e.message
                    $stderr.puts e.backtrace
                    API.assert{false}
                    return
                rescue
                    $stderr.puts $!.message
                    $stderr.puts $!.backtrace
                    responseInfo = ''
                    response = ''
                end
                return_async_nothrow(name, pattern, responseInfo, response,
                                     timeout, transId, pid)
            when MESSAGE_SEND_SYNC
                begin
                    response = function.call(SYNC, name, pattern,
                                             requestInfo, request,
                                             timeout, priority, transId, pid)
                    if response.kind_of?(Array)
                        API.assert{response.length == 2}
                        responseInfo = response[0]
                        response = response[1]
                        if not responseInfo.kind_of?(String)
                            responseInfo = ''
                        end
                    else
                        responseInfo = ''
                    end
                    if not response.kind_of?(String)
                        response = ''
                    end
                rescue ReturnSyncException
                    return
                rescue ReturnAsyncException => e
                    $stderr.puts e.message
                    $stderr.puts e.backtrace
                    API.assert{false}
                    return
                rescue
                    $stderr.puts $!.message
                    $stderr.puts $!.backtrace
                    responseInfo = ''
                    response = ''
                end
                return_sync_nothrow(name, pattern, responseInfo, response,
                                    timeout, transId, pid)
            else
                raise MessageDecodingException
            end
        end

        def poll
            ready = false
            while ready == false
                result = IO.select([@socket], nil, [@socket])
                if result[2].length > 0
                    return nil
                end
                if result[0].length > 0
                    ready = true
                end
            end

            data = recv('')
            if data.bytesize == 0
                return nil
            end

            loop do
                i = 0; j = 4
                command = data[i, j].unpack('L')[0]
                case command
                when MESSAGE_INIT
                    i += j; j = 4
                    prefixSize = data[i, j].unpack('L')[0]
                    i += j; j = prefixSize + 4 + 4 + 1
                    tmp = data[i, j].unpack("Z#{prefixSize}LLc")
                    @prefix = tmp[0]
                    @timeoutAsync = tmp[1]
                    @timeoutSync = tmp[2]
                    @priorityDefault = tmp[3]
                    i += j
                    if i != data.length
                        raise MessageDecodingException
                    end
                    return
                when MESSAGE_SEND_ASYNC, MESSAGE_SEND_SYNC
                    i += j; j = 4
                    nameSize = data[i, j].unpack('L')[0]
                    i += j; j = nameSize + 4
                    tmp = data[i, j].unpack("Z#{nameSize}L")
                    name = tmp[0]
                    patternSize = tmp[1]
                    i += j; j = patternSize + 4
                    tmp = data[i, j].unpack("Z#{patternSize}L")
                    pattern = tmp[0]
                    requestInfoSize = tmp[1]
                    i += j; j = requestInfoSize + 1 + 4
                    tmp = data[i, j].unpack("a#{requestInfoSize}xL")
                    requestInfo = tmp[0]
                    requestSize = tmp[1]
                    i += j; j = requestSize + 1 + 4 + 1 + 16 + 4
                    tmp = data[i, j].unpack("a#{requestSize}xLca16L")
                    request = tmp[0]
                    timeout = tmp[1]
                    priority = tmp[2]
                    transId = tmp[3]
                    pidSize = tmp[4]
                    i += j; j = pidSize
                    pid = data[i, j].unpack("a#{pidSize}")[0]
                    i += j
                    if i != data.length
                        raise MessageDecodingException
                    end
                    data.clear()
                    callback(command, name, pattern, requestInfo, request,
                             timeout, priority, transId, binary_to_term(pid))
                when MESSAGE_RECV_ASYNC, MESSAGE_RETURN_SYNC
                    i += j; j = 4
                    responseInfoSize = data[i, j].unpack('L')[0]
                    i += j; j = responseInfoSize + 1 + 4
                    tmp = data[i, j].unpack("a#{responseInfoSize}xL")
                    responseInfo = tmp[0]
                    responseSize = tmp[1]
                    i += j; j = responseSize + 1 + 16
                    tmp = data[i, j].unpack("a#{responseSize}xa16")
                    response = tmp[0]
                    transId = tmp[1]
                    i += j
                    if i != data.length
                        raise MessageDecodingException
                    end
                    return [responseInfo, response, transId]
                when MESSAGE_RETURN_ASYNC
                    i += j; j = 16
                    transId = data[i, j].unpack('a16')[0]
                    i += j
                    if i != data.length
                        raise MessageDecodingException
                    end
                    return transId
                when MESSAGE_RETURNS_ASYNC
                    i += j; j = 4
                    transIdCount = data[i, j].unpack('L')[0]
                    i += j; j = 16 * transIdCount
                    transIdList = data[i, j].unpack('a16' * transIdCount)
                    i += j
                    if i != data.length
                        raise MessageDecodingException
                    end
                    return transIdList
                when MESSAGE_KEEPALIVE
                    send(term_to_binary(:keepalive))
                    i += j
                    if i < data.length
                        raise MessageDecodingException
                    end
                    data.slice!(0, i)
                    if data.length > 0
                        if IO.select([@socket], nil, nil, 0).nil?
                            next
                        end
                    end
                else
                    raise MessageDecodingException
                end

                ready = false
                while ready == false
                    result = IO.select([@socket], nil, [@socket])
                    if result[2].length > 0
                        return nil
                    end
                    if result[0].length > 0
                        ready = true
                    end
                end
    
                data = recv(data)
                if data.bytesize == 0
                    return nil
                end
            end
        end

        def binary_key_value_parse(binary)
            result = {}
            data = binary.split(NULL.chr)
            (0...(data.length)).step(2).each do |i|
                value = result[data[i]]
                if value == nil
                    result[data[i]] = data[i + 1]
                elsif value.kind_of?(Array)
                    value << data[i + 1]
                else
                    result[data[i]] = [value, data[i + 1]]
                end
            end
            result
        end

        def request_http_qs_parse(request)
            binary_key_value_parse(request)
        end

        def info_key_value_parse(message_info)
            binary_key_value_parse(message_info)
        end

        private :return_async_nothrow
        private :return_sync_nothrow
        private :callback
        private :binary_key_value_parse
        private

        def send(data)
            if @use_header
                data = [data.length].pack('N') + data
            end
            @socket.write(data)
        end

        def recv(data)
            if @use_header
                while data.length < 4
                    fragment = @socket.readpartial(@size)
                    data += fragment
                end
                total = data[0,4].unpack('N')[0]
                data.slice!(0..3)
                while data.length < total
                    fragment = @socket.readpartial(@size)
                    data += fragment
                end
            else
                ready = true
                while ready == true
                    fragment = @socket.readpartial(@size)
                    data += fragment
                    ready = (fragment.bytesize == @size)
    
                    if ready
                        ready = ! IO.select([@socket], nil, nil, 0).nil?
                    end
                end
            end
            data
        end

        MESSAGE_INIT           = 1
        MESSAGE_SEND_ASYNC     = 2
        MESSAGE_SEND_SYNC      = 3
        MESSAGE_RECV_ASYNC     = 4
        MESSAGE_RETURN_ASYNC   = 5
        MESSAGE_RETURN_SYNC    = 6
        MESSAGE_RETURNS_ASYNC  = 7
        MESSAGE_KEEPALIVE      = 8

        NULL = 0

        def self.assert
            raise 'Assertion failed !' unless yield # if $DEBUG
        end

        def self.getenv(key)
            ENV[key] or raise InvalidInputException
        end
    end

    class InvalidInputException < Exception
    end

    class ReturnSyncException < Exception
    end

    class ReturnAsyncException < Exception
    end

    class MessageDecodingException < Exception
    end
end

