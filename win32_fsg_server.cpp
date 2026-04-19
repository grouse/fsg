HANDLE g_generate_mutex;

struct HttpBuilder {
    SOCKET recipient;
    char buffer[100*2048];
    i32 written = 0;
};

struct GeneratorThreadData {
    String output;
    String src_dir;
    bool build_drafts;
};

void append_stringf(HttpBuilder *hb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);


    i32 available = sizeof hb->buffer - hb->written;
    i32 required = vsnprintf(hb->buffer + hb->written, available-1, fmt, args);
    if (required > available-1) {
        send_data(hb->recipient, hb->buffer, hb->written);
        hb->written = 0;

        if (required > (i32)sizeof hb->buffer) {
            char *buffer = (char*)malloc(required);
            i32 length = vsnprintf(buffer, required, fmt, args);
            send_data(hb->recipient, buffer, length);
            free(buffer);
        } else {
            hb->written = vsnprintf(hb->buffer, sizeof hb->buffer-1, fmt, args);
        }
    } else {
        hb->written += required;
    }

    va_end(args);
}

void append_string(HttpBuilder *hb, String str)
{
    i32 available = MIN((i32)sizeof hb->buffer - hb->written, str.length);
    i32 rest = str.length - available;

    memcpy(hb->buffer+hb->written, str.data, available);
    hb->written += available;

    if (rest > 0) {
        send_data(hb->recipient, hb->buffer, hb->written);
        hb->written = 0;

        append_string(hb, { str.data, str.length-rest });
    }
}

String http_403_body = "<html><body><h1>Error: 403 - Forbidden</h1></body></html>";
String http_404_body = "<html><body><h1>Error: 404 - File not found</h1></body></html>";

bool send_data(SOCKET dst_socket, const char *data, i32 size)
{
    i32 bytes_sent = 0;
    do {
        i32 result = send(dst_socket, data, size, 0);
        if (result == SOCKET_ERROR) {
            LOG_ERROR("send gave error: %ld", WSAGetLastError());
            return false;
        }

        bytes_sent += result;
        size -= result;
        data += result;
    } while (bytes_sent < size);

    return true;
}

bool send_data(SOCKET dst_socket, String str)
{
    return send_data(dst_socket, str.data, str.length);
}


void send_header(SOCKET dst_socket, i32 code, String content_type, i32 content_length)
{
    HttpBuilder sb{ dst_socket, 0, 0 };
    append_stringf(&sb, "HTTP/1.1 %d ", code);

    switch (code) {
    case 200: append_string(&sb, "OK"); break;
    case 403: append_string(&sb, "Forbidden"); break;
    case 404: append_string(&sb, "Not Found"); break;
    }

    append_string(&sb, "\n");

    if (starts_with(content_type, "text") || content_type == "application/javascript") {
    	append_stringf(&sb, "Content-Type: %.*s;charset=UTF-8\n", content_type.length, content_type.data);
    } else {
        append_stringf(&sb, "Content-Type: %.*s\n", content_type.length, content_type.data);
    }

    if (content_length > 0) {
        append_stringf(&sb, "Content-Length: %d\n", content_length);
    }

    append_string(&sb, "Server: FSG\n");
    append_string(&sb, "Accept-Ranges: bytes\n");
    append_string(&sb, "Connection: close\n");

    append_string(&sb, "\n");

    if (sb.written > 0) {
        send_data(sb.recipient, sb.buffer, sb.written);
    }
}

DWORD generate_proc(void *data)
{
    GeneratorThreadData *gtd = (GeneratorThreadData*)data;

    char *sz_src_dir = sz_string(gtd->src_dir, mem_dynamic);
    HANDLE h = CreateFileA(
        sz_src_dir,
        GENERIC_READ | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    defer{ CloseHandle(h); };

    DWORD buffer[2028];
    DWORD bytes = 0;

    while (true) {
        bool result = ReadDirectoryChangesW(
            h,
            buffer, sizeof buffer,
            true, FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytes, NULL, NULL);

        if (!result) break;

        WaitForSingleObject(g_generate_mutex, INFINITE);
        Sleep(1000);
        generate_src_dir(gtd->output, gtd->src_dir, gtd->build_drafts);
        html_dirty = true;
        ReleaseMutex(g_generate_mutex);
    }

    return 0;
}

void run_server()
{
    g_generate_mutex = CreateMutex(NULL, FALSE, NULL);
    GeneratorThreadData gen_thread_data{ output, src_dir, build_drafts };
    HANDLE gen_thread = CreateThread(NULL, 8*1024*1024, &generate_proc, &gen_thread_data, 0, nullptr);
    (void)gen_thread;

    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != NO_ERROR) {
        LOG_ERROR("WSAStartup failed: %d", result);
        return 1;
    }
    defer { WSACleanup(); };

    SOCKET lis_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lis_socket == INVALID_SOCKET) {
        LOG_ERROR("socket creation failed: %ld", WSAGetLastError());
        return 1;
    }
    defer { closesocket(lis_socket); };

    sockaddr_in service{};
    service.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &service.sin_addr);
    service.sin_port = htons(80);

    if (bind(lis_socket, (SOCKADDR*)&service, sizeof service) == SOCKET_ERROR) {
        LOG_ERROR("bind failed: %ld", WSAGetLastError());
        return 1;
    }

    if (listen(lis_socket, 1) == SOCKET_ERROR) {
        LOG_ERROR("listen failed: %ld", WSAGetLastError());
        return 1;
    }

    while (true) {
        LOG_INFO("waiting incoming connection");
        SOCKET in_socket = accept(lis_socket, NULL, NULL);
        if (in_socket == INVALID_SOCKET) {
            LOG_ERROR("accept failed with error: %ld", WSAGetLastError());
            return 1;
        }
        LOG_INFO("client connected");

        defer {
            LOG_INFO("closing socket");
            closesocket(in_socket);
        };

        char buffer[2048];

        // TODO(jesper): actually support having the http request across multiple recv calls
        // to do that I need to partially parse the headers as I receive, checking for Content-Length
        // and double newline header terminators
        int result = recv(in_socket, buffer, sizeof buffer, 0);
        while (result > 0) {
            LOG_INFO("received bytes: %d", result);
            Lexer lexer{ buffer, buffer+result, "http socket" };

            Token t;
            if (!require_next_token(&lexer, TOKEN_IDENTIFIER, &t)) break;
            if (t.str == "GET") {
                if (!require_next_token(&lexer, TOKEN_WHITESPACE, LEXER_FLAG_NONE, &t)) break;

                t = next_token(&lexer, LEXER_FLAG_NONE);
                String path = t.str;

                t = next_token(&lexer, LEXER_FLAG_NONE);
                while (t.type != TOKEN_EOF && t.type != TOKEN_WHITESPACE) {
                    path.length += t.str.length;
                    t = next_token(&lexer, LEXER_FLAG_NONE);
                }

                if (!eat_until(&lexer, TOKEN_NEWLINE, &t)) break;

                while (t.type != TOKEN_EOF) {
                    t = next_token(&lexer);
                    if (t.type == TOKEN_NEWLINE) break;
                    if (!eat_until(&lexer, TOKEN_NEWLINE, &t)) break;
                }

                LOG_INFO("finished parsing http header with %d bytes left", (i32)(lexer.end - lexer.at));

                canonicalise_path(path);
                if (path == "\\") path = "\\index.html";

                LOG_INFO("HTTP GET: %.*s", STRFMT(path));

                path = join_path(output, path, mem_dynamic);
                defer{ free(path.data); };

                String args{};
                for (i32 i = 0; i < path.length; i++) {
                    if (path[i] == '?') {
                        args = { &path[i+1], path.length-i-1 };
                        path.length = i;
                    }
                }

                String content_type;
                if (ends_with(path, ".html")) {
                    content_type = "text/html";
                } else if (ends_with(path, ".css")) {
                    content_type = "text/css";
                } else if (ends_with(path, ".js")) {
                    content_type = "application/javascript";
                } else if (ends_with(path, ".ttf") ||
                           ends_with(path, ".woff2"))
                {
                    content_type = "application/octet-stream";
                } else if (ends_with(path, ".png")) {
                    content_type = "image/png";
                } else if (ends_with(path, ".jpg")) {
                    content_type = "image/jpeg";
                } else {
                    LOG_INFO("requested unsupported file type: %.*s", path.length, path.data);
                    send_header(in_socket, 403, "text/html", http_403_body.length);
                    send_data(in_socket, http_403_body);
                    goto req_end;
                }

                WaitForSingleObject(g_generate_mutex, INFINITE);

                FileInfo contents = read_file(path, mem_dynamic);
                if (!contents.data) {
                    LOG_INFO("respond: 404: %.*s", path.length, path.data);
                    send_header(in_socket, 404, "text/html", http_404_body.length);
                    send_data(in_socket, http_404_body);
                    goto req_end;
                }
                defer{ destroy_string(contents); };

                LOG_INFO("respond: 200");
                send_header(in_socket, 200, content_type, contents.length);
                send_data(in_socket, contents);

                ReleaseMutex(g_generate_mutex);

                result = recv(in_socket, buffer, sizeof buffer, 0);
            }
        }
    req_end:;
    }
}
