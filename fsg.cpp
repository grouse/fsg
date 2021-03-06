#include <stdio.h>
#include <stdarg.h>

#include <functional>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

extern "C" void* malloc(size_t size);

#define MAX(a, b) (a) > (b) ? (a) : (b)
#define MIN(a, b) (a) > (b) ? (b) : (a)

#define swap(a, b) { auto tmp = a; a = b; b = tmp; }

using u8 = unsigned char;
using i32 = int;
using u32 = unsigned int;
using i64 = signed long long;

template <typename F>
struct Defer {
    Defer(F f) : f(f) {}
    ~Defer() { f(); }
    F f;
};

template <typename F>
Defer<F> defer_create( F f ) { return Defer<F>( f ); }

#define defer__(line) defer_ ## line
#define defer_(line) defer__( line )

struct DeferDummy {};
template<typename F> 
Defer<F> operator+(DeferDummy, F&& f) { return defer_create<F>(std::forward<F>(f)); }

#define defer auto defer_( __LINE__ ) = DeferDummy( ) + [&]( )

template<typename T>
struct Array {
    T *data;
    i32 count;

    T& operator[](i32 i) { return data[i]; }
    T* begin() { return &data[0]; }
    T* end() { return &data[count]; }
};

template<typename T>
struct DynamicArray : public Array<T> {
    i32 capacity;
};

template<typename T>
void array_add(DynamicArray<T> *arr, T& e)
{
    if (arr->count+1 > arr->capacity) {
        i32 new_capacity = arr->capacity == 0 ? 1 : arr->capacity*2;
        arr->capacity = new_capacity;
        arr->data = (T*)realloc(arr->data, new_capacity*sizeof e);
    }
    
    arr->data[arr->count++] = e;
}

struct String {
    char *bytes = nullptr;
    i32 length = 0;
    
    String() = default;
    
    template<i32 N>
    String(const char(&str)[N])
    {
        length = N - 1;
        bytes = (char*)str;
    }
    
    String(const char *str, i32 length)
    {
        this->length = length;
        bytes = (char*)str;
    }
    
    char& operator[](i32 i)
    {
        return bytes[i];
    }
    
    bool operator==(String rhs)
    {
        return length == rhs.length && (bytes == rhs.bytes || memcmp(bytes, rhs.bytes, length) == 0);
    }
    
    bool operator!=(String rhs)
    {
        return length != rhs.length || (bytes != rhs.bytes && memcmp(bytes, rhs.bytes, length) != 0);
    }
    
    bool operator>(String rhs)
    {
        return strncmp(bytes, rhs.bytes, MIN(length, rhs.length)) > 0;
    }
    
    bool operator<(String rhs)
    {
        return strncmp(bytes, rhs.bytes, MIN(length, rhs.length)) < 0;
    }
};

struct TagProperty {
    String key;
    String value;
};

void destroy_string(String str)
{
    free(str.bytes);
}

#define STRFMT(str) (str).length, (str).bytes
void fsg_log(const char *fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(buffer, sizeof buffer-2, fmt, args);
    va_end(args);

    if (length < (i32)sizeof buffer-2) {
        buffer[length] = '\n';
        buffer[length+1] = '\0';
        OutputDebugStringA(buffer);
        printf("%s", buffer);
    }
}


char* sz_string(String str)
{
    char *sz_str = (char*)malloc(str.length+1);
    memcpy(sz_str, str.bytes, str.length);
    sz_str[str.length] = '\0';
    return sz_str;
}

enum LexerFlags : u32 {
    LEXER_FLAG_NONE = 0,

    LEXER_FLAG_EAT_NEWLINE = 1 << 0,
    LEXER_FLAG_EAT_WHITESPACE = 1 << 1,
    LEXER_FLAG_EAT_COMMENT = 1 << 2,
    
    // TODO(jesper): this is a super dirty hack that probably requires some pretty
    // long winded rewrites of the whole way I deal with templates and sections etc to
    // generate the page
    LEXER_FLAG_ENABLE_ANCHOR = 1 << 3,

    LEXER_FLAGS_DEFAULT = LEXER_FLAG_EAT_WHITESPACE,
};

struct Lexer {
    char *at;
    char *end;
    
    String debug_name;
    LexerFlags flags = LEXER_FLAGS_DEFAULT;
};

#define LOG_ERROR(...) fsg_log(__VA_ARGS__)
#define LOG_INFO(...) fsg_log(__VA_ARGS__)
#define PARSE_ERROR(lexer, msg, ...)\
    LOG_ERROR("parse error: %.*s: " msg, STRFMT((lexer)->debug_name), __VA_ARGS__)


enum FsgTokenType : u8 {
    TOKEN_START = 127,
    
    TOKEN_ANCHOR,
    TOKEN_CODE_BLOCK,
    TOKEN_CODE_INLINE,

    TOKEN_IDENTIFIER,
    TOKEN_COMMENT,
    
    TOKEN_WHITESPACE,
    TOKEN_NEWLINE,
    
    TOKEN_EOF,
};
const char* token_type_str(FsgTokenType type)
{
    switch (type) {
    case TOKEN_IDENTIFIER: return "identifier";
    case TOKEN_COMMENT: return "comment";
    case TOKEN_WHITESPACE: return "whitespace";
    case TOKEN_NEWLINE: return "newline";
    case TOKEN_EOF: return "EOF";
    default: return "?";
    }
}

struct Token {
    FsgTokenType type;
    String str;
};


bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_numeric(char c)
{
    return c >= '0' && c <= '9';
}

bool starts_with(String lhs, String rhs)
{
    return lhs.length >= rhs.length && memcmp(lhs.bytes, rhs.bytes, rhs.length) == 0;
}

bool ends_with(String lhs, String rhs)
{
    return lhs.length >= rhs.length && memcmp(lhs.bytes+lhs.length-rhs.length, rhs.bytes, rhs.length) == 0;
}

bool is_comment_start(Lexer *lexer)
{
    return (i32)(lexer->end - lexer->at) >= 4 && starts_with(String{ lexer->at, 4 }, "<!--");
}

bool is_comment_end(Lexer *lexer)
{
    return (i32)(lexer->end - lexer->at) >= 3 && starts_with(String{ lexer->at, 3 }, "-->");
}

i32 bytes_remain(Lexer *lexer)
{
    return (i32)(lexer->end - lexer->at);
}

bool starts_with(Lexer *lexer, String str)
{
    return starts_with(String{ lexer->at, bytes_remain(lexer) }, str);
}


Token next_token(Lexer *lexer, LexerFlags flags)
{
    while (lexer->at < lexer->end) {
        if (lexer->at[0] == ' ' || lexer->at[0] == '\t') {
            Token result;
            result.type = TOKEN_WHITESPACE;
            result.str.bytes = lexer->at++;
            
            while (lexer->at < lexer->end &&
                   (lexer->at[0] == ' ' || lexer->at[0] == '\t'))
            {
                lexer->at++;
            }
            
            result.str.length = (i32)(lexer->at - result.str.bytes);
            if (!(flags & LEXER_FLAG_EAT_WHITESPACE)) return result;
        } else if (lexer->at[0] == '\n' || lexer->at[0] == '\r') {
            Token result;
            result.type = TOKEN_NEWLINE;
            result.str.bytes = lexer->at;
            
            if (lexer->at[0] == '\r') lexer->at++;
            if (lexer->at[0] == '\n') lexer->at++;
            
            result.str.length = (i32)(lexer->at - result.str.bytes);
            if (!(flags & LEXER_FLAG_EAT_NEWLINE)) return result;
        } else if (is_comment_start(lexer)) {
            lexer->at += 4;
            
            Token result;
            result.type = TOKEN_COMMENT;
            result.str.bytes = lexer->at;
            
            i32 comment_level = 1;
            while (lexer->at < lexer->end) {
                if (is_comment_end(lexer)) {
                    lexer->at += 3;
                    if (--comment_level <= 0) break;
                } else if (is_comment_start(lexer)) {
                    comment_level++;
                }
                
                lexer->at++;
            }
            
            result.str.length = (i32)(lexer->at - result.str.bytes);
            result.str.length = (i32)(lexer->at - result.str.bytes - 3);
            if (!(flags & LEXER_FLAG_EAT_COMMENT)) return result;
        } else if (starts_with(lexer, "```")) {
            Token result;
            result.type = TOKEN_CODE_BLOCK;
            
            lexer->at += 3;
            while (lexer->at < lexer->end && 
                   (*lexer->at == ' ' || *lexer->at == '\n' || *lexer->at == '\r')) 
            {
                lexer->at++;
            }
            
            result.str.bytes = lexer->at++;
            while (lexer->at < lexer->end && !starts_with(lexer, "```")) {
                lexer->at++;
            }
            result.str.length = (i32)(lexer->at - result.str.bytes);
            
            if (starts_with(lexer, "```")) lexer->at += 3;
            return result;
        } else if (starts_with(lexer, "`")) {
            Token result;
            result.type = TOKEN_CODE_INLINE;
            
            lexer->at += 1;
            result.str.bytes = lexer->at++;
            while (lexer->at < lexer->end && *lexer->at != '`') {
                lexer->at++;
            }
            result.str.length = (i32)(lexer->at - result.str.bytes);
            if (*lexer->at == '`') lexer->at += 1;
            return result;
        } else if (lexer->flags & LEXER_FLAG_ENABLE_ANCHOR && 
                   starts_with(lexer, "<a")) 
        {
            Token result;
            result.type = TOKEN_ANCHOR;
            result.str.bytes = lexer->at++;
            
            while (lexer->at < lexer->end) {
                if (starts_with(lexer, "</a>")) {
                    lexer->at += 4;
                    break;
                }
                lexer->at++;
            }
            
            result.str.length = (i32)(lexer->at - result.str.bytes);
            return result;
        } else if (is_alpha(lexer->at[0]) || is_numeric(lexer->at[0]) || (u8)lexer->at[0] >= 128) {
            Token result;
            result.type = TOKEN_IDENTIFIER;
            result.str.bytes = lexer->at++;
            
            while (lexer->at < lexer->end) {
                if (!is_alpha(lexer->at[0]) && !is_numeric(lexer->at[0]) && (u8)lexer->at[0] < 128) {
                    break;
                }
                
                lexer->at++;
            }
            
            result.str.length = (i32)(lexer->at - result.str.bytes);
            return result;
        } else {
            Token result;
            result.type = (FsgTokenType)lexer->at[0];
            result.str.bytes = lexer->at++;
            result.str.length = 1;
            return result;
        }
    }
    
    Token result = {};
    result.type = TOKEN_EOF;
    return result;
}

Token next_token(Lexer *lexer)
{
    return next_token(lexer, lexer->flags);
}


Token peek_next_token(Lexer *lexer)
{
    Lexer copy = *lexer;
    return next_token(&copy, lexer->flags);
}

bool require_next_token(Lexer *lexer, FsgTokenType type, Token *out)
{
    *out = next_token(lexer, lexer->flags);
    if (out->type != type) {
        PARSE_ERROR(lexer, "unexpected token. expected '%s', got '%.*s'", token_type_str(type), STRFMT(out->str));
        return false;
    }
    return true;
}

bool require_next_token(Lexer *lexer, FsgTokenType type, LexerFlags flags, Token *out)
{
    *out = next_token(lexer, flags);
    if (out->type != type) {
        PARSE_ERROR(lexer, "unexpected token. expected '%s', got '%.*s'", token_type_str(type), STRFMT(out->str));
        return false;
    }
    
    return true;
}

bool require_next_token(Lexer *lexer, char type, Token *out)
{
    *out = next_token(lexer, lexer->flags);
    if (out->type != type) {
        PARSE_ERROR(lexer, "unexpected token. expected '%c', got '%.*s'", type, STRFMT(out->str));
        return false;
    }
    return true;

}

bool eat_until(Lexer *lexer, FsgTokenType type, Token *out)
{
    Token t;
    do {
        t = next_token(lexer, LEXER_FLAG_NONE);
    } while (t.type != type && t.type != TOKEN_EOF);
    *out = t;
    
    if (t.type != type) {
        PARSE_ERROR(lexer, "unexpected EOF. expected '%s'", token_type_str(type));
        return false;
    }
    return true;
}

bool eat_until(Lexer *lexer, char c, Token *out)
{
    Token t;
    do {
        t = next_token(lexer, LEXER_FLAG_NONE);
    } while (t.type != c && t.type != TOKEN_EOF);
    *out = t;

    if (t.type != c) {
        PARSE_ERROR(lexer, "unexpected EOF. expected '%c'", c);
        return false;
    }
    return true;
}

Array<TagProperty> parse_html_tag_properties(String tag)
{
    DynamicArray<TagProperty> properties{};

    char *at = tag.bytes;
    char *end = tag.bytes + tag.length;

    at++;
    while (at < end) {
        if (*at == ' ' || *at == '>') break;
        at++;
    }

    if (*at == '>') return properties;

    at++;
    while (at < end) {
        TagProperty property{};
        
        if (*at == '>') goto end;
        while (at < end && *at == ' ') at++;

        if (is_alpha(at[0])) {
            property.key.bytes = at++;
            while (at < end) {
                if (*at == '>' || *at == '=' || *at == ' ') break;
                at++;
            }
            property.key.length = (i32)(at - property.key.bytes);

            if (at < end && *at == '=') {
                at += 2;
                property.value.bytes = at++;
                while (at < end) {
                    if (*at == '"') break;
                    at++;
                }
                property.value.length = (i32)(at - property.value.bytes);
                at++;
            }
        }

        if (property.key.length > 0) {
            array_add(&properties, property);
        }
        
    }

end:
    return properties;
}

String parse_html_tag_inner(String tag)
{
    char *at = tag.bytes;
    char *end = tag.bytes + tag.length;

    at++;
    while (at < end) {
        if (*at == '>') break;
        at++;
    }
    
    at++;

    String inner{};
    inner.bytes = at++;
    
    while (at < end) {
        if (*at == '<') break;
        at++;
    }
    
    inner.length = (i32)(at - inner.bytes);
    return inner;
}




struct HttpBuilder {
    SOCKET recipient;
    char buffer[100*2048];
    i32 written = 0;
};

struct StringBuilder {
    struct Block {
        char data[4096];
        Block *next = nullptr;
        i32 written = 0;
    };
    
    Block head = {};
    Block *current = &head;
};


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

bool send_data(SOCKET dst_socket, String data)
{
    return send_data(dst_socket, data.bytes, data.length);
}

void append_string(StringBuilder *sb, String str)
{
    i32 available = MIN((i32)sizeof sb->current->data - sb->current->written, str.length);
    i32 rest = str.length - available;

    memcpy(sb->current->data+sb->current->written, str.bytes, available);
    sb->current->written += available;
    
    i32 written = available;
    while(rest > 0) {
        StringBuilder::Block *block = (StringBuilder::Block*)malloc(sizeof(StringBuilder::Block));
        memset(block, 0, sizeof *block);
        
        sb->current->next = block;
        sb->current = block;
        
        i32 to_write = MIN((i32)sizeof sb->current->data, rest);
        memcpy(block->data, str.bytes+written, to_write);
        block->written += to_write;
        
        rest -= to_write;
        written += to_write;
    }
}

void append_char(StringBuilder *sb, char c)
{
    i32 available = sizeof sb->current->data - sb->current->written;
    
    if (available < 1) {
        StringBuilder::Block *block = (StringBuilder::Block*)malloc(sizeof(StringBuilder::Block));
        memset(block, 0, sizeof *block);

        sb->current->next = block;
        sb->current = block;
    }

    *(sb->current->data+sb->current->written) = c;
    sb->current->written += 1;
}

void append_stringf(StringBuilder *builder, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    i32 available = sizeof builder->current->data - builder->current->written;
    i32 length = vsnprintf(builder->current->data + builder->current->written, available-1, fmt, args);

    if (length > available-1) {
        char *buffer = (char*)malloc(length+1);
        vsnprintf(buffer, length+1, fmt, args);

        append_string(builder, String{ buffer, length });
    } else {
        builder->current->written += length;
    }

    va_end(args);
}

void append_escape_html(StringBuilder *sb, String str)
{
    for (i32 i = 0; i < str.length; i++) {
        switch (str[i]) {
        case '<': append_string(sb, "&lt;"); break;
        case '>': append_string(sb, "&gt;"); break;
        default: append_char(sb, str[i]); break;
        }
    }
}

String to_string(StringBuilder *sb)
{
    i32 size = 0;
    StringBuilder::Block *block = &sb->head;
    do {
        size += block->written;
        block = block->next;
    } while (block);
    
    String str;
    str.bytes = (char*)malloc(size);
    
    block = &sb->head;
    do {
        memcpy(str.bytes+str.length, block->data, block->written);
        str.length += block->written;
        block = block->next;
    } while (block);
    
    return str;
}

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
    
    memcpy(hb->buffer+hb->written, str.bytes, available);
    hb->written += available;
    
    if (rest > 0) {
        send_data(hb->recipient, hb->buffer, hb->written);
        hb->written = 0;
        
        append_string(hb, { str.bytes, str.length-rest });
    }
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
    	append_stringf(&sb, "Content-Type: %.*s;charset=UTF-8\n", content_type.length, content_type.bytes);
    } else {
        append_stringf(&sb, "Content-Type: %.*s\n", content_type.length, content_type.bytes);
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

String join_path(String lhs, String rhs)
{
    i32 required = lhs.length + rhs.length;
    if (lhs[lhs.length]-1 != '\\' && rhs[0] != '\\') {
        required += 1;
    }
    
    String result;
    result.bytes = (char*)malloc(required);
    
    memcpy(result.bytes, lhs.bytes, lhs.length);
    result.length = lhs.length;
    
    if (lhs[lhs.length-1] != '\\' && rhs[0] != '\\') {
        result.length += 1;
        result[result.length-1] = '\\';
    }

    memcpy(result.bytes+result.length, rhs.bytes, rhs.length);
    result.length += rhs.length;
    return result;
}

String join_url(String lhs, String rhs)
{
    i32 required = lhs.length + rhs.length;
    if (lhs[lhs.length]-1 != '/' && rhs[0] != '/') {
        required += 1;
    }

    String result;
    result.bytes = (char*)malloc(required);

    memcpy(result.bytes, lhs.bytes, lhs.length);
    result.length = lhs.length;

    if (lhs[lhs.length-1] != '/' && rhs[0] != '/') {
        result.length += 1;
        result[result.length-1] = '/';
    }

    memcpy(result.bytes+result.length, rhs.bytes, rhs.length);
    result.length += rhs.length;
    return result;
}


void canonicalise_path(String path)
{   
    for (i32 i = 0; i < path.length; i++) {
        path.bytes[i] = path.bytes[i] == '/' ? '\\' : path.bytes[i];
    }
}

DynamicArray<String> list_files(String dir)
{
    DynamicArray<String> files{};

    i32 length = dir.length;

    char sz_path[2048];
    memcpy(sz_path, dir.bytes, length);
    canonicalise_path(String{ sz_path, length });
    
    bool eslash = dir[length-1] == '\\';
    String root{ sz_path, eslash ? length : length+1 };

	if (!eslash) sz_path[length++] = '\\';
    sz_path[length++] = '*';
    sz_path[length++] = '.';
    sz_path[length++] = '*';
    sz_path[length++] = '\0';
    

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(sz_path, &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    defer { FindClose(h); };
    
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        } else {
            String path = join_path(root, String{ fd.cFileName, (i32)strlen(fd.cFileName) });
            array_add(&files, path);

        }
    } while(FindNextFile(h, &fd));
    
    return files;
}

char* win32_system_error_message(DWORD error)
{
    static char buffer[2048];
    DWORD result = FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        error,
        0,
        buffer,
        sizeof buffer,
        NULL);
    (void)result;
    return buffer;
}

String read_entire_file(String path)
{
    String result{};
    
    char *sz_path = sz_string(path);
    defer { free(sz_path); };
    
    HANDLE file = CreateFileA(
        sz_path, 
        GENERIC_READ, 
        FILE_SHARE_READ, 
        nullptr, 
        OPEN_EXISTING, 
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    
    if (file == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        LOG_ERROR("invalid file handle: %s - %d:%s", sz_path, code, win32_system_error_message(code));
        return {};
    }
    defer{ CloseHandle(file); };

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size)) {
        LOG_ERROR("failed getting file size");
        return {};
    }

    result.length = (i32)file_size.QuadPart;
    result.bytes = (char*)malloc(result.length);

    DWORD bytes_read;
    if (!ReadFile(file, result.bytes, result.length, &bytes_read, nullptr) ||
        bytes_read != (DWORD)result.length) 
    {
        LOG_ERROR("failed reading file");
        return {};
    }
    
    return result;
}

void remove_files(String in_dir)
{
    DynamicArray<String> folders{};
    array_add(&folders, in_dir);
    
    for (i32 i = 0; i < folders.count; i++) {
        String dir = folders[i];
        i32 length = dir.length;

        char sz_dir[2048];
        memcpy(sz_dir, dir.bytes, length);
        canonicalise_path(String{ sz_dir, length });

        bool eslash = dir[length-1] == '\\';
        String root{ sz_dir, eslash ? length : length+1 };

        if (!eslash) sz_dir[length++] = '\\';
        sz_dir[length++] = '*';
        sz_dir[length++] = '.';
        sz_dir[length++] = '*';
        sz_dir[length++] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(sz_dir, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            defer { FindClose(h); };

            do {
                if (fd.cFileName[0] == '.') continue;

                String path = join_path(root, String{ fd.cFileName, (i32)strlen(fd.cFileName) });
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    array_add(&folders, path);
                } else {
                    char *sz_file = sz_string(path);
                    defer{
                        free(sz_file);
                        destroy_string(path);
                    };

                    if (DeleteFileA(sz_file) == 0) {
                        LOG_ERROR("failed removing file: '%s', win32 error: '%s'", sz_file, win32_system_error_message(GetLastError())); 
                    } else {
                        LOG_INFO("deleted folder: %s", sz_file);
                    }

                }
            } while(FindNextFile(h, &fd));
        }
    }
    
#if 0
    for (i32 i = folders.count-1; i >= 1; i--) {
        char *sz_dir = sz_string(folders[i]);
        if (RemoveDirectoryA(sz_dir) == 0) {
            LOG_ERROR("failed removing directory: '%s', win32 error: '%s'", sz_dir, win32_system_error_message(GetLastError())); 
        } else {
            LOG_INFO("deleted dir: %s", sz_dir);
        }
        free(sz_dir);
    }
#endif
}

void write_file(String path, String content)
{
    char *sz_path = sz_string(path);
    defer { free(sz_path); };

    HANDLE file = CreateFileA(
        sz_path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    
    if (file == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == 3) {
            char *ptr = sz_path;
            char *end = ptr + strlen(ptr);
            
            while (ptr < end) {
                if (*ptr == '\\') {
                    *ptr = '\0';
                    defer{ *ptr = '\\'; };
                    
                    if (CreateDirectoryA(sz_path, NULL) == 0) {
                        DWORD create_dir_error = GetLastError();
                        if (create_dir_error != ERROR_ALREADY_EXISTS) {
                            LOG_ERROR("failed creating folder: %s, code: %d, msg: '%s'", sz_path, create_dir_error, win32_system_error_message(create_dir_error));
                            return;
                        }
                    }
                }
                ptr++;
            }
        } else {
            LOG_ERROR("failed creating file: '%s', code: %d, msg: '%s'", sz_path, code, win32_system_error_message(code));
            return;
        }
    }
    defer{ CloseHandle(file); };
    
    DWORD bytes_written;
    WriteFile(file, content.bytes, content.length, &bytes_written, nullptr);
}

void destroy_string_builder(StringBuilder *sb)
{
    StringBuilder::Block *block = sb->head.next;
    while (block) {
        StringBuilder::Block *next = block->next;
        free(block);
        block = next;
    }
}

bool is_identifier(Token t, String str)
{
    return t.type == TOKEN_IDENTIFIER && t.str == str;
}
    
struct Section {
    String name;
    i32 offset;
    i32 length;
};

struct Template {
    String name;
    String contents;
    Array<Section> sections;
};

struct Page {
    String path;
    String name;
    String title;
    String subtitle;
    String dst_section_name;
    String contents;
    Array<Section> sections;
    i32 tmpl_index = -1;
};

struct Post {
    String path;
    String title;
    String created;
    String url;
    bool draft;
    String brief;
    String content;
};

template<typename T>
T* array_find(Array<T> arr, T e)
{
    for (i32 i = 0; i < arr.count; i++) {
        if (arr.data[i] == e) return &arr.data[i];
    }
    return nullptr;
}

i32 find_template_index(Array<Template> templates, String name)
{
    for (i32 i = 0; i < templates.count; i++) {
        if (templates.data[i].name == name) return i;
    }
    return -1;

}

Template* find_template(Array<Template> templates, String name)
{
    for (i32 i = 0; i < templates.count; i++) {
        if (templates.data[i].name == name) return &templates.data[i];
    }
    return nullptr;

}


String copy_string(String rhs)
{
    String result;
    result.bytes = (char*)malloc(rhs.length);
    result.length = rhs.length;
    memcpy(result.bytes, rhs.bytes, rhs.length);
    return result;
}


struct GeneratorThreadData {
    String output;
    String src_dir;
    bool build_drafts;
};

void copy_files(String root, String folder, String dst)
{
    DynamicArray<String> files = list_files(join_path(root, folder));

    for (String p : files) {
        String contents = read_entire_file(p);
        defer{ destroy_string(contents); };

        String filename{ p.bytes+root.length, p.length-root.length };
        String out_file = join_path(dst, filename);
        defer{ destroy_string(out_file); };

        write_file(out_file, contents);
    }
}

HANDLE g_generate_mutex;
volatile bool html_dirty = false;

bool parse_string(Lexer *lexer, String *str_out, Token *t_out)
{
    Token t = peek_next_token(lexer);
    if (t.type == '"') {
        t = next_token(lexer);
        
        String str;
        str.bytes = t.str.bytes+1;
        if (!eat_until(lexer, '"', &t)) return false;
        str.length = t.str.bytes - str.bytes;
        
        *t_out = t;
        *str_out = str;
        return true;
    } else if (t.type == ';') {
        *str_out = String{};
        return true;
    } else {
        t = next_token(lexer);
        
        String str;
        str.bytes = t.str.bytes;
        
        while (t.type != TOKEN_EOF) {
            t = peek_next_token(lexer);
            if (t.type == ';') break;
            t = next_token(lexer);
        }
        str.length = t.str.bytes - str.bytes;
        
        *t_out = t;
        *str_out = str;
        return true;
    }
    
    return false;
}

bool parse_bool(Lexer *lexer, bool *bool_out, Token *t_out)
{
    String str;
    if (parse_string(lexer, &str, t_out)) {
        if (str == "yes" || str == "true") {
            *bool_out = true;
            return true;
        } else if (str == "no" || str == "false") {
            *bool_out = false;
            return true;
        }
        PARSE_ERROR(lexer, "unexpected identifier parsing bool: %.*s", STRFMT(str));
        return false;
    }
    return false;
}

void generate_src_dir(String output, String src_dir, bool build_drafts)
{
    DynamicArray<Template> templates{};
    DynamicArray<Post> posts{};
    DynamicArray<Page> pages{};
    
    String posts_src_path = join_path(src_dir, "_posts");
    String posts_dst_path = join_path(output, "posts");

    DynamicArray<String> page_files = list_files(src_dir);
    DynamicArray<String> post_files = list_files(posts_src_path);
    DynamicArray<String> template_files = list_files(join_path(src_dir, "_templates"));
    
    remove_files(output);
    
    copy_files(src_dir, "css", output);
    copy_files(src_dir, "img", output);
    copy_files(src_dir, "js", output);
    copy_files(src_dir, "fonts", output);
    
    for (String p : post_files) {
        Post post;
        Lexer lexer;
        Token t;
        char *ptr;
        StringBuilder content{};

        String filename{ p.bytes+posts_src_path.length+1, p.length-posts_src_path.length-1};

        String contents = read_entire_file(p);
        if (!contents.bytes) {
            LOG_ERROR("failed reading %.*s", p.length, p.bytes);
            goto next_post_file;
        }
        
        post = Post{};
        lexer = Lexer{ 
            contents.bytes, 
            contents.bytes+contents.length, 
            p, 
            (LexerFlags)(LEXER_FLAG_NONE | LEXER_FLAG_ENABLE_ANCHOR)
        };
        ptr = lexer.at;

        t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                Lexer fsg_lexer{ 
                    t.str.bytes, 
                    t.str.bytes+t.str.length, 
                    p,
                    (LexerFlags)(LEXER_FLAG_EAT_NEWLINE | LEXER_FLAG_EAT_WHITESPACE)
                };
                
                Token t2 = next_token(&fsg_lexer);
                if (is_identifier(t2, "fsg")) {
                    if (!require_next_token(&fsg_lexer, ':', &t2)) goto next_post_file;
                    
                    t2 = next_token(&fsg_lexer);
                    if (is_identifier(t2, "brief")) {
                        if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_post_file;
                        if (!require_next_token(&fsg_lexer, TOKEN_EOF, &t2)) goto next_post_file;
                        
                        post.brief = to_string(&content);
                    } else {
                        while (t2.type != TOKEN_EOF) {
                            if (is_identifier(t2, "title")) {
                                if (!parse_string(&fsg_lexer, &post.title, &t2)) goto next_post_file;
                                if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_post_file;
                            } else if (is_identifier(t2, "created")) {
                                if (!parse_string(&fsg_lexer, &post.created, &t2)) goto next_post_file;
                                if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_post_file;
                            } else if (is_identifier(t2, "draft")) {
                                if (!parse_bool(&fsg_lexer, &post.draft, &t2)) goto next_post_file;
                                if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_post_file;
                            } else {
                                PARSE_ERROR(
                                    &fsg_lexer, 
                                    "unexpected token. expected one of 'title' or 'date', got '%.*s'", 
                                    STRFMT(t2.str));
                                goto next_post_file;
                            }

                            t2 = next_token(&fsg_lexer);

                        }
                    }
                }
            } else if (t.type == TOKEN_CODE_BLOCK) {
                append_string(&content, "<code class=\"block\">");
                append_escape_html(&content, t.str);
                append_string(&content, "</code>");
            } else if (t.type == TOKEN_CODE_INLINE) {
                append_string(&content, "<code>");
                append_escape_html(&content, t.str);
                append_string(&content, "</code>");
            } else if (t.type == TOKEN_ANCHOR) {
                i32 length = (i32)(t.str.bytes - ptr);
                if (length > 0) append_string(&content, String{ ptr, length });
                
                Array<TagProperty> properties = parse_html_tag_properties(t.str);
                String inner = parse_html_tag_inner(t.str);
                    
                bool has_href = false;
                append_string(&content, "<a");
                
                for (TagProperty prop : properties) {
                    if (prop.key == "href") has_href = true;
                    append_stringf(&content, " %.*s=\"%.*s\"", STRFMT(prop.key), STRFMT(prop.value));
                }
                
                if (!has_href) append_stringf(&content, " href=\"%.*s\"", STRFMT(inner));
                append_stringf(&content, ">%.*s</a>", STRFMT(inner));
            } else {
                i32 length = (i32)(lexer.at - ptr);
                if (length > 0) append_string(&content, String{ ptr, length });
            }
            
            ptr = lexer.at;
            t = next_token(&lexer);
        }
        
        post.content = to_string(&content);
        if (post.brief.length == 0) post.brief = post.content;
        post.brief = post.content;
        
        post.path = join_path(posts_dst_path, filename);
        post.url = join_url("/posts", filename);
        array_add(&posts, post);
        
next_post_file:;
    }
    
    for (i32 i = 0; i < posts.count; i++) {
        for (i32 j = i; j > 0 && posts[j-1].created < posts[j].created; j--) {
            swap(posts[j], posts[j-1]);
        }
    }

    for (String p : template_files) {
        String contents = read_entire_file(p);
        if (!contents.bytes) {
            LOG_ERROR("failed reading %.*s", p.length, p.bytes);
            return;
        }

        Template tmpl{};
        tmpl.contents = contents;
        
        Section tail;
        i32 last_section_end = 0;


        String filename = p;
        while (filename.length > 0 && filename[filename.length-1] != '.') filename.length--;
        filename.length = filename.bytes[filename.length-1] == '.' ? filename.length-1 : filename.length;

        filename.bytes = filename.bytes + filename.length-1;
        while (filename.bytes > p.bytes && filename.bytes[0] != '\\') filename.bytes--;
        filename.bytes = *filename.bytes == '\\' ? filename.bytes+1 : filename.bytes;
        filename.length -= (i32)(filename.bytes-p.bytes);

        DynamicArray<Section> sections{};

        tmpl.name = filename;

        Lexer lexer{ contents.bytes, contents.bytes+contents.length, p };

        Token t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                char *comment_start = t.str.bytes-4;
                char *comment_end = t.str.bytes+t.str.length+3;

                
                Lexer fsg_lexer{ 
                    t.str.bytes, 
                    t.str.bytes+t.str.length, 
                    p,
                    (LexerFlags)(LEXER_FLAG_EAT_WHITESPACE | LEXER_FLAG_EAT_NEWLINE)
                };
                
                Token t2 = next_token(&fsg_lexer);
                if (is_identifier(t2, "fsg")) {
                    Section section{};

                    if (!require_next_token(&fsg_lexer, ':', &t2)) goto next_tmpl_file;
                    
                    t2 = next_token(&fsg_lexer);
                    while (t2.type != TOKEN_EOF) {

                        if (is_identifier(t2, "section")) {
                            if (!parse_string(&fsg_lexer, &section.name, &t2)) goto next_tmpl_file;
                            if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_tmpl_file;
                        } else {
                            PARSE_ERROR(&fsg_lexer, "unexpected token. expected one of 'section', got '%.*s'", STRFMT(t2.str));
                            goto next_tmpl_file;
                        } 
                        
                        t2 = next_token(&fsg_lexer);

                    }

                    section.offset = last_section_end;
                    section.length = (i32)(comment_start-contents.bytes-last_section_end);
                    last_section_end = (i32)(comment_end - contents.bytes);
                    array_add(&sections, section);
                }

            }

            t = next_token(&lexer);
        }
        
        tail = Section{ "", last_section_end, (i32)(lexer.end - (contents.bytes + last_section_end)) };
        if (tail.length > 0) array_add(&sections, tail);

        tmpl.sections = sections;
        array_add(&templates, tmpl);

next_tmpl_file:;
    }

    for (String p : page_files) {
        Page page{};
        
        String filename{ p.bytes+src_dir.length+1, p.length-src_dir.length-1};
        String out_file = join_path(output, filename);
        
        page.name = filename;
        page.path = out_file;

        String contents = read_entire_file(p);
        if (!contents.bytes) {
            LOG_ERROR("failed reading: %.*s", p.length, p.bytes);
            continue;
        }
        
        page.contents = contents;

        
        DynamicArray<Section> sections{};
        Section tail{};

        i32 last_section_end = 0;
        
        Lexer lexer{ contents.bytes, contents.bytes+contents.length, p };
        Token t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                char *comment_start = t.str.bytes-4;
                char *comment_end = t.str.bytes+t.str.length+3;

                Lexer fsg_lexer{ 
                    t.str.bytes, 
                    t.str.bytes+t.str.length, 
                    p, 
                    (LexerFlags)(LEXER_FLAG_EAT_WHITESPACE | LEXER_FLAG_EAT_NEWLINE)
                };
                
                Token t2 = next_token(&fsg_lexer);
                if (is_identifier(t2, "fsg")) {
                    Section section{};
                    
                    if (!require_next_token(&fsg_lexer, ':', &t2)) goto next_page_file;

                    t2 = next_token(&fsg_lexer);
                    while (t2.type != TOKEN_EOF) {
                        if (is_identifier(t2, "template")) {
                            if (page.tmpl_index == -1) {
                                if (!require_next_token(&fsg_lexer, TOKEN_IDENTIFIER, &t2)) goto next_page_file;
                                page.tmpl_index = find_template_index(templates, t2.str);
                                
                                if (!require_next_token(&fsg_lexer, '.', &t2)) goto next_page_file;
                                if (!require_next_token(&fsg_lexer, TOKEN_IDENTIFIER, &t2)) goto next_page_file;
                                page.dst_section_name = t2.str;
                                
                                if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_page_file;
                            } else {
                                PARSE_ERROR(&fsg_lexer, "duplicate template properties");
                                goto next_page_file;
                            }
                        } else if (is_identifier(t2, "section")) {
                            if (!parse_string(&fsg_lexer, &section.name, &t2)) goto next_page_file;
                            if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_page_file;
                        } else if (is_identifier(t2, "title")) {
                            if (!parse_string(&fsg_lexer, &page.title, &t2)) goto next_page_file;
                            if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_page_file;
                        } else if (is_identifier(t2, "subtitle")) {
                            if (!parse_string(&fsg_lexer, &page.subtitle, &t2)) goto next_page_file;
                            if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_page_file;
                        } else {
                            PARSE_ERROR(
                                &fsg_lexer, 
                                "unexpected identifier. expected one of 'template', 'section' - got '%.*s'", 
                                STRFMT(t2.str));
                            goto next_page_file;
                        }

                        t2 = next_token(&fsg_lexer);
                    }
                    
                    section.offset = last_section_end;
                    section.length = (i32)(comment_start-contents.bytes-last_section_end);
                    last_section_end = (i32)(comment_end-contents.bytes);
                    array_add(&sections, section);
                }
            }
            
            t = next_token(&lexer);
        }
        
        tail = Section{ "", last_section_end, (i32)(lexer.end - (contents.bytes + last_section_end)) };
        if (tail.length > 0) array_add(&sections, tail);
        
        page.sections = sections;
        array_add(&pages, page);
        
next_page_file:;
    }
    
    for (Page page : pages) {
        StringBuilder sb{};
        defer{ destroy_string_builder(&sb); };
        
        Template *tmpl = &templates[page.tmpl_index];
        for (Section s : tmpl->sections) {
            append_string(&sb, String{ tmpl->contents.bytes+s.offset, s.length });
            if (s.name == page.dst_section_name) {
                for (Section s2 : page.sections) {
                    append_string(&sb, String{ page.contents.bytes+s2.offset, s2.length });
                    if (s2.name == "posts.brief" || s2.name == "posts.full") {
                        String tmpl_name = s2.name == "posts.brief" ? String{ "post_brief_inline" } : String{ "post_full_block" };
                        Template *brief_tmpl = find_template(templates, tmpl_name);
                        
                        if (!brief_tmpl) {
                            LOG_ERROR("invalid template: post_brief_inline");
                            continue;
                        }
                        for (Post post : posts) {
                            if (!build_drafts && post.draft) continue; 
                            
                            for (Section s3 : brief_tmpl->sections) {
                                append_string(&sb, String{ brief_tmpl->contents.bytes+s3.offset, s3.length });
                                if (s3.name == "post.created") {
                                    append_string(&sb, post.created);
                                } else if (s3.name == "post.title") {
                                    append_string(&sb, post.title);
                                } else if (s3.name == "post.url") {
                                    append_string(&sb, post.url);
                                } else if (s3.name == "post.brief") {
                                    append_string(&sb, post.brief);
                                } else if (s3.name == "post.content") {
                                    append_string(&sb, post.content);
                                } else if(s3.name.length > 0) {
                                    LOG_ERROR("unhandled section '%.*s'", STRFMT(s3.name));
                                }
                            }
                        }
                    } else if (s2.name.length > 0) {
                        LOG_ERROR("unhandled section '%.*s' in page '%.*s'", STRFMT(s2.name), STRFMT(page.name));
                    }
                }
            } else if (s.name == "page.title") {
                append_string(&sb, page.title);
            } else if (s.name == "page.subtitle") {
                append_string(&sb, page.subtitle);
            } else if (s.name.length > 0){
                LOG_ERROR("unhandled section '%.*s' in template '%.*s'", STRFMT(s.name), STRFMT(tmpl->name));
            }
        }
        
        String out_contents = to_string(&sb);
        defer{ destroy_string(out_contents); };
        
        write_file(page.path, out_contents);
    }
    
    Template *post_tmpl = find_template(templates, "post");
    if (post_tmpl) {
    	for (Post post : posts) {
            if (!build_drafts && post.draft) continue; 

            String out_file = post.path;
            
            StringBuilder sb{};
            defer{ destroy_string_builder(&sb); };
            
            for (Section s : post_tmpl->sections) {
                append_string(&sb, String{ post_tmpl->contents.bytes+s.offset, s.length });
                if (s.name.length > 0) {
                    if (s.name == "post.content") {
                        append_string(&sb, post.content);
                    } else if (s.name == "post.created") {
                        append_string(&sb, post.created);
                    } else if (s.name == "post.title") {
                        append_string(&sb, post.title);
                    }
                }
            }
            
            String out_contents = to_string(&sb);
            defer{ destroy_string(out_contents); };
            
            write_file(out_file, out_contents);
        }
    }
}

DWORD generate_proc(void *data)
{
    GeneratorThreadData *gtd = (GeneratorThreadData*)data;
    
    char *sz_src_dir = sz_string(gtd->src_dir);
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        LOG_INFO("usage: fsg generate|server -src=path -output=path [-drafts]");
        return 1;
    }
    
    String output{};
    String src_dir{};
    
    enum {
        RUN_MODE_NONE,
        RUN_MODE_GENERATE,
        RUN_MODE_SERVER
    } run_mode = RUN_MODE_NONE;
    
    bool build_drafts = false;
    
    for (i32 i = 1; i < argc; i++) {
        String a{ argv[i], (i32)strlen(argv[i]) };
        if (starts_with(a, "generate")) {
            if (run_mode != 0) {
                LOG_ERROR("can only supply one of generate|server");
                return 1;
            }
            run_mode = RUN_MODE_GENERATE;
        } else if (starts_with(a, "server")) {
            if (run_mode != 0) {
                LOG_ERROR("can only supply one of generate|server");
                return 1;
            }
            run_mode = RUN_MODE_SERVER;
        } else if (starts_with(a, "-output=")) {
            output = { a.bytes+strlen("-output="), a.length-(i32)strlen("-output=") };
        } else if (starts_with(a, "-src=")) {
            src_dir = { a.bytes+strlen("-src="), a.length-(i32)strlen("-src=") };
        } else if (starts_with(a, "-drafts")) {
            build_drafts = true;
        } else {
            LOG_INFO("usage: fsg -output=path");
        }
    }
    
    if (run_mode == 0) {
        LOG_ERROR("must supply one of generate|server");
        return 1;
    }
    
    if (output.length == 0) {
        LOG_ERROR("empty output path");
        return 1;
    }
    
    if (src_dir.length == 0) {
        LOG_ERROR("empty src_dir path");
        return 1;
    }
    
    canonicalise_path(output);
    canonicalise_path(src_dir);
    
    generate_src_dir(output, src_dir, build_drafts);
    
    if (run_mode == RUN_MODE_SERVER) {
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

                    path = join_path(output, path);
                    defer{ free(path.bytes); };
                    
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
                        LOG_INFO("requested unsupported file type: %.*s", path.length, path.bytes);
                        send_header(in_socket, 403, "text/html", http_403_body.length);
                        send_data(in_socket, http_403_body);
                        goto req_end;
                    }

                    WaitForSingleObject(g_generate_mutex, INFINITE);

                    String contents = read_entire_file(path);
                    if (!contents.bytes) {
                        LOG_INFO("respond: 404: %.*s", path.length, path.bytes);
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
    
    return 0;
}
    