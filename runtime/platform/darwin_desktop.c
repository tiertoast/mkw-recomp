// Desktop integration for the standalone executable.
#include "platform.h"
#include <CoreText/CoreText.h>
#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8); }

int plat_open_folder(const char *path) {
    struct stat st;
    char abs[PATH_MAX];
    if (!realpath(path, abs) || stat(abs, &st) || !S_ISDIR(st.st_mode)) return 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)abs, strlen(abs), true);
    if (!url) return 0;
    char text[PATH_MAX * 3 + 16];
    int ok = CFStringGetCString(CFURLGetString(url), text, sizeof text, kCFStringEncodingUTF8) && SDL_OpenURL(text);
    CFRelease(url);
    return ok;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    // Match an actual glyph in the requested script. Language-only descriptor matching can return a
    // bitmap/emoji face, which the menu's TrueType atlas cannot rasterize.
    static const struct { const char *lang; UniChar glyph; } scripts[] = {
        {"ja", 0x3042}, {"ko", 0xd55c}, {"zh-Hans", 0x7b80}, {"zh-Hant", 0x7e41}, {"th", 0x0e44},
    };
    UniChar glyph = 0;
    for (unsigned i = 0; i < sizeof scripts / sizeof *scripts; i++)
        if (!strcmp(lang, scripts[i].lang)) glyph = scripts[i].glyph;
    if (!glyph) return 0;
    CFStringRef language = CFStringCreateWithCString(NULL, lang, kCFStringEncodingUTF8);
    CFStringRef sample = CFStringCreateWithCharacters(NULL, &glyph, 1);
    // PingFangUI in macOS 26 uses Apple-only outline tables. Heiti ships standard TrueType outlines.
    CTFontRef base = !strcmp(lang, "zh-Hans") ? CTFontCreateWithName(CFSTR("STHeitiSC-Light"), 16, NULL)
                   : !strcmp(lang, "zh-Hant") ? CTFontCreateWithName(CFSTR("STHeitiTC-Light"), 16, NULL)
                   : CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 16, language);
    CTFontRef font = base && sample ? CTFontCreateForStringWithLanguage(base, sample, CFRangeMake(0, 1), language) : NULL;
    // CoreText can render more formats than the menu. Check for real glyf or CFF outlines before handing it a file.
    CFArrayRef tables = font ? CTFontCopyAvailableTables(font, kCTFontTableOptionNoOptions) : NULL;
    CFRange range = CFRangeMake(0, tables ? CFArrayGetCount(tables) : 0);
    int outlines = tables && (CFArrayContainsValue(tables, range, (const void *)(uintptr_t)kCTFontTableGlyf) ||
                              CFArrayContainsValue(tables, range, (const void *)(uintptr_t)kCTFontTableCFF));
    CGGlyph index;
    CFURLRef url = outlines && CTFontGetGlyphsForCharacters(font, &glyph, &index, 1)
                   ? CTFontCopyAttribute(font, kCTFontURLAttribute) : NULL;
    int ok = url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)out, cap) && !access(out, R_OK);
    if (url) CFRelease(url);
    if (tables) CFRelease(tables);
    if (font) CFRelease(font);
    if (base) CFRelease(base);
    if (sample) CFRelease(sample);
    if (language) CFRelease(language);
    return ok;
}

// Finder opens executable .command files in Terminal. Quote both paths as literal shell operands.
static void shell_path(FILE *f, const char *path) {
    fputc('\'', f);
    for (; *path; path++) {
        if (*path == '\'') fputs("'\\''", f);
        else fputc(*path, f);
    }
    fputc('\'', f);
}
int plat_desktop_shortcut(const char *name, const char *exe, const char *dir, char *out, size_t cap) {
    const char *desk = SDL_GetUserFolder(SDL_FOLDER_DESKTOP);
    char path[PATH_MAX], temp[PATH_MAX];
    if (!desk || !*name || strchr(name, '/') || exe[0] != '/' || dir[0] != '/') return 0;
    int n = snprintf(path, sizeof path, "%s/%s.command", desk, name);
    if (n < 0 || (size_t)n >= sizeof path || (out && (size_t)n >= cap)) return 0;
    n = snprintf(temp, sizeof temp, "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof temp) return 0;
    int fd = mkstemp(temp);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "w");
    int ok = 0;
    if (f) {
        fputs("#!/bin/sh\ncd ", f);
        shell_path(f, dir);
        fputs(" || exit 1\nexec ", f);
        shell_path(f, exe);
        fputc('\n', f);
        ok = !ferror(f) && !fchmod(fd, 0700);
        if (fclose(f)) ok = 0;
    } else close(fd);
    if (!ok || rename(temp, path)) { unlink(temp); return 0; }
    if (out) strcpy(out, path);
    return 1;
}

int plat_url_scheme(const char *scheme, const char *label, const char *exe, const char *arg) {
    (void)scheme; (void)label; (void)exe; (void)arg;
    return 0; // Launch Services needs a registered application bundle to receive open-URL events.
}
