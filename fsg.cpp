#include "core/core.h"
#include "core/string.h"
#include "core/file.h"

#include <stdio.h>
#include <stdarg.h>

#include <functional>

struct TagProperty {
    String key;
    String value;
};


#define PARSE_ERRORF(lexer, msg, ...)\
    LOG_ERROR("parse error: %.*s: " msg, STRFMT((lexer)->debug_name), __VA_ARGS__)

#define PARSE_ERROR(lexer, msg)\
    LOG_ERROR("parse error: %.*s: " msg, STRFMT((lexer)->debug_name))


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
            result.str.data = lexer->at++;

            while (lexer->at < lexer->end &&
                   (lexer->at[0] == ' ' || lexer->at[0] == '\t'))
            {
                lexer->at++;
            }

            result.str.length = (i32)(lexer->at - result.str.data);
            if (!(flags & LEXER_FLAG_EAT_WHITESPACE)) return result;
        } else if (lexer->at[0] == '\n' || lexer->at[0] == '\r') {
            Token result;
            result.type = TOKEN_NEWLINE;
            result.str.data = lexer->at;

            if (lexer->at[0] == '\r') lexer->at++;
            if (lexer->at[0] == '\n') lexer->at++;

            result.str.length = (i32)(lexer->at - result.str.data);
            if (!(flags & LEXER_FLAG_EAT_NEWLINE)) return result;
        } else if (is_comment_start(lexer)) {
            lexer->at += 4;

            Token result;
            result.type = TOKEN_COMMENT;
            result.str.data = lexer->at;

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

            result.str.length = (i32)(lexer->at - result.str.data);
            result.str.length = (i32)(lexer->at - result.str.data - 3);
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

            result.str.data = lexer->at++;
            while (lexer->at < lexer->end && !starts_with(lexer, "```")) {
                lexer->at++;
            }
            result.str.length = (i32)(lexer->at - result.str.data);

            if (starts_with(lexer, "```")) lexer->at += 3;
            return result;
        } else if (starts_with(lexer, "`")) {
            Token result;
            result.type = TOKEN_CODE_INLINE;

            lexer->at += 1;
            result.str.data = lexer->at++;
            while (lexer->at < lexer->end && *lexer->at != '`') {
                lexer->at++;
            }
            result.str.length = (i32)(lexer->at - result.str.data);
            if (*lexer->at == '`') lexer->at += 1;
            return result;
        } else if (lexer->flags & LEXER_FLAG_ENABLE_ANCHOR &&
                   starts_with(lexer, "<a"))
        {
            Token result;
            result.type = TOKEN_ANCHOR;
            result.str.data = lexer->at++;

            while (lexer->at < lexer->end) {
                if (starts_with(lexer, "</a>")) {
                    lexer->at += 4;
                    break;
                }
                lexer->at++;
            }

            result.str.length = (i32)(lexer->at - result.str.data);
            return result;
        } else if (is_alpha(lexer->at[0]) || is_number(lexer->at[0]) || (u8)lexer->at[0] >= 128) {
            Token result;
            result.type = TOKEN_IDENTIFIER;
            result.str.data = lexer->at++;

            while (lexer->at < lexer->end) {
                if (!is_alpha(lexer->at[0]) && !is_number(lexer->at[0]) && (u8)lexer->at[0] < 128) {
                    break;
                }

                lexer->at++;
            }

            result.str.length = (i32)(lexer->at - result.str.data);
            return result;
        } else {
            Token result;
            result.type = (FsgTokenType)lexer->at[0];
            result.str.data = lexer->at++;
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
        PARSE_ERRORF(lexer, "unexpected token. expected '%s', got '%.*s'", token_type_str(type), STRFMT(out->str));
        return false;
    }
    return true;
}

bool require_next_token(Lexer *lexer, FsgTokenType type, LexerFlags flags, Token *out)
{
    *out = next_token(lexer, flags);
    if (out->type != type) {
        PARSE_ERRORF(lexer, "unexpected token. expected '%s', got '%.*s'", token_type_str(type), STRFMT(out->str));
        return false;
    }

    return true;
}

bool require_next_token(Lexer *lexer, char type, Token *out)
{
    *out = next_token(lexer, lexer->flags);
    if (out->type != type) {
        PARSE_ERRORF(lexer, "unexpected token. expected '%c', got '%.*s'", type, STRFMT(out->str));
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
        PARSE_ERRORF(lexer, "unexpected EOF. expected '%s'", token_type_str(type));
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
        PARSE_ERRORF(lexer, "unexpected EOF. expected '%c'", c);
        return false;
    }
    return true;
}

Array<TagProperty> parse_html_tag_properties(String tag)
{
    DynamicArray<TagProperty> properties{};

    char *at = tag.data;
    char *end = tag.data + tag.length;

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
            property.key.data = at++;
            while (at < end) {
                if (*at == '>' || *at == '=' || *at == ' ') break;
                at++;
            }
            property.key.length = (i32)(at - property.key.data);

            if (at < end && *at == '=') {
                at += 2;
                property.value.data = at++;
                while (at < end) {
                    if (*at == '"') break;
                    at++;
                }
                property.value.length = (i32)(at - property.value.data);
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
    char *at = tag.data;
    char *end = tag.data + tag.length;

    at++;
    while (at < end) {
        if (*at == '>') break;
        at++;
    }

    at++;

    String inner{};
    inner.data = at++;

    while (at < end) {
        if (*at == '<') break;
        at++;
    }

    inner.length = (i32)(at - inner.data);
    return inner;
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


String join_url(String lhs, String rhs)
{
    i32 required = lhs.length + rhs.length;
    if (lhs[lhs.length]-1 != '/' && rhs[0] != '/') {
        required += 1;
    }

    String result;
    result.data = (char*)malloc(required);

    memcpy(result.data, lhs.data, lhs.length);
    result.length = lhs.length;

    if (lhs[lhs.length-1] != '/' && rhs[0] != '/') {
        result.length += 1;
        result[result.length-1] = '/';
    }

    memcpy(result.data+result.length, rhs.data, rhs.length);
    result.length += rhs.length;
    return result;
}

void canonicalise_path(String path)
{
    // for (i32 i = 0; i < path.length; i++) {
    //     path.data[i] = path.data[i] == '/' ? '\\' : path.data[i];
    // }
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
    DynamicArray<String> tags;
    String url;
    bool draft;
    String brief;
    String content;
};

struct Tag {
    String str;
    DynamicArray<Post> posts;
};

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

void copy_files(String root, String folder, String dst)
{
    SArena scratch = tl_scratch_arena();

    DynamicArray<String> files = list_files(join_path(root, folder, scratch), scratch);

    for (String p : files) {
        SArena mem_file = tl_scratch_arena(scratch);

        FileInfo contents = read_file(p, mem_file);

        String filename{ p.data+root.length, p.length-root.length };
        String out_file = join_path(dst, filename, mem_file);

        write_file(out_file, contents.data, contents.size);
    }
}

volatile bool html_dirty = false;

bool parse_string(Lexer *lexer, String *str_out, Token *t_out)
{
    Token t = peek_next_token(lexer);
    if (t.type == '"') {
        t = next_token(lexer);

        String str;
        str.data = t.str.data+1;
        if (!eat_until(lexer, '"', &t)) return false;
        str.length = t.str.data - str.data;

        *t_out = t;
        *str_out = str;
        return true;
    } else if (t.type == ';') {
        *str_out = String{};
        return true;
    } else {
        t = next_token(lexer);

        String str;
        str.data = t.str.data;

        while (t.type != TOKEN_EOF) {
            t = peek_next_token(lexer);
            if (t.type == ';') break;
            t = next_token(lexer);
        }
        str.length = t.str.data - str.data;

        *t_out = t;
        *str_out = str;
        return true;
    }

    return false;
}

bool parse_string_list(Lexer *lexer, DynamicArray<String> *strs_out, Token *t_out)
{
    Token t = peek_next_token(lexer);
    while (t.type != TOKEN_EOF) {
        if (t.type == '"') {
            t = next_token(lexer);

            String str;
            str.data = t.str.data+1;
            if (!eat_until(lexer, '"', &t)) return false;
            str.length = t.str.data - str.data;

            array_add(strs_out, str);
            *t_out = t;
        } else if (t.type == ',') {
            t = next_token(lexer);
        } else if (t.type == ';') {
            return true;
        } else {
            t = next_token(lexer);

            String str;
            str.data = t.str.data;
            while (t.type != TOKEN_EOF) {
                t = peek_next_token(lexer);
                if (t.type == ';' || t.type == ',') break;
                t = next_token(lexer);
            }
            str.length = t.str.data - str.data;
            array_add(strs_out, str);
            *t_out = t;
        }

        t = peek_next_token(lexer);
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
        PARSE_ERRORF(lexer, "unexpected identifier parsing bool: '%.*s'", STRFMT(str));
        return false;
    }
    return false;
}

void append_post(StringBuilder *sb, Template *tmpl, Post post)
{
    for (Section s : tmpl->sections) {
        append_string(sb, String{ tmpl->contents.data+s.offset, s.length });
        if (s.name == "post.created") {
            append_string(sb, post.created);
        } else if (s.name == "post.title") {
            append_string(sb, post.title);
        } else if (s.name == "post.url") {
            append_string(sb, post.url);
        } else if (s.name == "post.brief") {
            append_string(sb, post.brief);
        } else if (s.name == "post.content") {
            append_string(sb, post.content);
        } else if (s.name == "post.tags") {
            if (post.tags.count > 0) {
                append_string(sb, "<i class=\"fa fa-tag\"></i>");

                for (i32 i = 0; i < post.tags.count-1; i++) {
                    append_stringf(
                        sb,
                        "<a href=\"/posts/tag/%.*s.html\">%.*s</a>, ",
                        STRFMT(post.tags[i]),
                        STRFMT(post.tags[i]));
                }

                append_stringf(
                    sb,
                    "<a href=\"/posts/tag/%.*s.html\">%.*s</a>",
                    STRFMT(post.tags[post.tags.count-1]),
                    STRFMT(post.tags[post.tags.count-1]));

            }
        } else if(s.name.length > 0) {
            LOG_ERROR("unhandled section '%.*s'", STRFMT(s.name));
        }
    }
}

void generate_src_dir(String output, String src_dir, bool build_drafts)
{
    SArena scratch = tl_scratch_arena();

    DynamicArray<Template> templates{};
    DynamicArray<Post> posts{};
    DynamicArray<Page> pages{};
    DynamicArray<Tag> tags{};

    String posts_src_path = join_path(src_dir, "_posts", mem_dynamic);
    String posts_dst_path = join_path(output, "posts", mem_dynamic);

    DynamicArray<String> page_files = list_files(src_dir, mem_dynamic);
    DynamicArray<String> post_files = list_files(posts_src_path, mem_dynamic);
    DynamicArray<String> template_files = list_files(join_path(src_dir, "_templates", mem_dynamic), mem_dynamic);

    remove_files(output);

    copy_files(src_dir, "css", output);
    copy_files(src_dir, "img", output);
    copy_files(src_dir, "js", output);
    copy_files(src_dir, "fonts", output);

    for (String p : post_files) {
        String filename{ p.data+posts_src_path.length+1, p.length-posts_src_path.length-1};

        FileInfo contents = read_file(p, mem_dynamic);
        if (!contents.data) {
            LOG_ERROR("failed reading %.*s", p.length, p.data);
            continue;
        }

        StringBuilder content{};

        Lexer lexer{
            (char*)contents.data,
            (char*)contents.data+contents.size,
            p,
            (LexerFlags)(LEXER_FLAG_NONE | LEXER_FLAG_ENABLE_ANCHOR)
        };
        char *ptr = lexer.at;

        Post post{};

        Token t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                Lexer fsg_lexer{
                    t.str.data,
                    t.str.data+t.str.length,
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

                        post.brief = create_string(&content, mem_dynamic);
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
                            } else if (is_identifier(t2, "tags")) {
                                if (!parse_string_list(&fsg_lexer, &post.tags, &t2)) goto next_post_file;
                                if (!require_next_token(&fsg_lexer, ';', &t2)) goto next_post_file;
                            } else {
                                PARSE_ERRORF(
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
                i32 length = (i32)(t.str.data - ptr);
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

        post.content = create_string(&content, mem_dynamic);
        if (post.brief.length == 0) post.brief = post.content;

        post.path = join_path(posts_dst_path, filename, mem_dynamic);
        post.url = join_url("/posts", filename);
        array_add(&posts, post);

        for (i32 i = 0; i < post.tags.count; i++) {
            for (Tag &t : tags) {
                if (t.str == post.tags[i]) {
                    array_add(&t.posts, post);
                    goto next_post_file;
                }
            }

            Tag t{};
            t.str = post.tags[i];
            array_add(&t.posts, post);
            array_add(&tags, t);
        }

next_post_file:;
    }

    for (i32 i = 0; i < posts.count; i++) {
        for (i32 j = i; j > 0 && posts[j-1].created < posts[j].created; j--) {
            SWAP(posts[j], posts[j-1]);
        }
    }

    for (String p : template_files) {
        FileInfo contents = read_file(p, mem_dynamic);
        if (!contents.data) {
            LOG_ERROR("failed reading %.*s", p.length, p.data);
            return;
        }

        Template tmpl{};
        tmpl.contents = String{ (char*)contents.data, contents.size };

        Section tail;
        i32 last_section_end = 0;


        String filename = p;
        while (filename.length > 0 && filename[filename.length-1] != '.') filename.length--;
        filename.length = filename.data[filename.length-1] == '.' ? filename.length-1 : filename.length;

        filename.data = filename.data + filename.length-1;
        while (filename.data > p.data && filename.data[0] != '\\') filename.data--;
        filename.data = *filename.data == '\\' ? filename.data+1 : filename.data;
        filename.length -= (i32)(filename.data-p.data);

        DynamicArray<Section> sections{};

        tmpl.name = filename;

        Lexer lexer{ (char*)contents.data, (char*)contents.data+contents.size, p };

        Token t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                char *comment_start = t.str.data-4;
                char *comment_end = t.str.data+t.str.length+3;


                Lexer fsg_lexer{
                    t.str.data,
                    t.str.data+t.str.length,
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
                            PARSE_ERRORF(&fsg_lexer, "unexpected token. expected one of 'section', got '%.*s'", STRFMT(t2.str));
                            goto next_tmpl_file;
                        }

                        t2 = next_token(&fsg_lexer);

                    }

                    section.offset = last_section_end;
                    section.length = (i32)(comment_start-(char*)contents.data-last_section_end);
                    last_section_end = (i32)(comment_end - (char*)contents.data);
                    array_add(&sections, section);
                }

            }

            t = next_token(&lexer);
        }

        tail = Section{ "", last_section_end, (i32)(lexer.end - ((char*)contents.data + last_section_end)) };
        if (tail.length > 0) array_add(&sections, tail);

        tmpl.sections = sections;
        array_add(&templates, tmpl);

next_tmpl_file:;
    }

    for (String p : page_files) {
        Page page{};

        String filename{ p.data+src_dir.length+1, p.length-src_dir.length-1};
        String out_file = join_path(output, filename, mem_dynamic);

        page.name = filename;
        page.path = out_file;

        FileInfo contents = read_file(p, mem_dynamic);
        if (!contents.data) {
            LOG_ERROR("failed reading: %.*s", p.length, p.data);
            continue;
        }

        page.contents = String{ (char*)contents.data, contents.size };


        DynamicArray<Section> sections{};
        Section tail{};

        i32 last_section_end = 0;

        Lexer lexer{ (char*)contents.data, (char*)contents.data+contents.size, p };
        Token t = next_token(&lexer);
        while (t.type != TOKEN_EOF) {
            if (t.type == TOKEN_COMMENT) {
                char *comment_start = t.str.data-4;
                char *comment_end = t.str.data+t.str.length+3;

                Lexer fsg_lexer{
                    t.str.data,
                    t.str.data+t.str.length,
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
                            PARSE_ERRORF(
                                &fsg_lexer,
                                "unexpected identifier. expected one of 'template', 'section' - got '%.*s'",
                                STRFMT(t2.str));
                            goto next_page_file;
                        }

                        t2 = next_token(&fsg_lexer);
                    }

                    section.offset = last_section_end;
                    section.length = (i32)(comment_start-(char*)contents.data-last_section_end);
                    last_section_end = (i32)(comment_end-(char*)contents.data);
                    array_add(&sections, section);
                }
            }

            t = next_token(&lexer);
        }

        tail = Section{ "", last_section_end, (i32)(lexer.end - ((char*)contents.data + last_section_end)) };
        if (tail.length > 0) array_add(&sections, tail);

        page.sections = sections;
        array_add(&pages, page);

next_page_file:;
    }


    Template *brief_tmpl = find_template(templates, "post_brief_inline");
    Template *brief_block_tmpl = find_template(templates, "post_brief_block");
    Template *full_tmpl = find_template(templates, "post_full_block");

    Template *tag_tmpl = find_template(templates, "posts_tag");

    if (tag_tmpl) {
        for (Tag tag : tags) {
            SArena scratch = tl_scratch_arena();
            StringBuilder sb{ .alloc = scratch };

            for (Section s : tag_tmpl->sections) {
                append_string(&sb, String{ tag_tmpl->contents.data+s.offset, s.length });

                if (s.name == "posts.brief" || s.name == "posts.full") {
                    Template *post_tmpl = s.name == "posts.brief" ? brief_block_tmpl : full_tmpl;

                    for (Post post : tag.posts) {
                        if (!build_drafts && post.draft) continue;
                        append_post(&sb, post_tmpl, post);
                    }
                } else if (s.name == "tag.str") {
                    append_string(&sb, tag.str);
                } else if (s.name.length > 0) {
                    LOG_ERROR("unhandled section '%.*s' in template '%.*s'", STRFMT(s.name), STRFMT(tag_tmpl->name));
                }
            }

            String path = join_path(output, stringf(mem_dynamic, "\\posts\\tag\\%.*s.html", STRFMT(tag.str)), mem_dynamic);
            //defer{ destroy_string(path); };

            write_file(path, &sb);
        }
    }


    for (Page page : pages) {
        SArena scratch = tl_scratch_arena();
        StringBuilder sb{ .alloc = scratch };

        Template *tmpl = &templates[page.tmpl_index];
        for (Section s : tmpl->sections) {
            append_string(&sb, String{ tmpl->contents.data+s.offset, s.length });
            if (s.name == page.dst_section_name) {
                for (Section s2 : page.sections) {
                    append_string(&sb, String{ page.contents.data+s2.offset, s2.length });
                    if (s2.name == "posts.brief" || s2.name == "posts.full") {
                        Template *post_tmpl = s2.name == "posts.brief" ? brief_tmpl : full_tmpl;

                        for (Post post : posts) {
                            if (!build_drafts && post.draft) continue;
                            append_post(&sb, post_tmpl, post);
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

        write_file(page.path, &sb);
    }

    Template *post_tmpl = find_template(templates, "post");
    if (post_tmpl) {
    	for (Post post : posts) {
            if (!build_drafts && post.draft) continue;

            SArena scratch = tl_scratch_arena();
            StringBuilder sb{ .alloc = scratch };
            append_post(&sb, post_tmpl, post);

            write_file(post.path, &sb);
        }
    }
}


int main(Array<String> args)
{
    if (args.count < 3) {
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

    for (i32 i = 0; i < args.count; i++) {
        String a = args[i];
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
            output = { a.data+strlen("-output="), a.length-(i32)strlen("-output=") };
        } else if (starts_with(a, "-src=")) {
            src_dir = { a.data+strlen("-src="), a.length-(i32)strlen("-src=") };
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

    // if (run_mode == RUN_MODE_SERVER) {
    //     extern void run_server();
    //     run_server();
    // }

    return 0;
}
