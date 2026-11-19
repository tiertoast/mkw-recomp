// Darwin wall time, zone name and the user's language and regional format.
#include "platform.h"
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

uint64_t plat_unix_time_ns(void) { return clock_gettime_nsec_np(CLOCK_REALTIME); }

void plat_local_time(struct tm *out) {
    time_t now = time(NULL);
    localtime_r(&now, out);
}

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return timezone;
}

int plat_time_zone(char *out, size_t cap) {
    if (!cap) return 0;
    CFTimeZoneRef zone = CFTimeZoneCopySystem();
    if (!zone) return 0;
    int ok = CFStringGetCString(CFTimeZoneGetName(zone), out, cap, kCFStringEncodingUTF8);
    CFRelease(zone);
    return ok;
}

static char ui_language[128], user_locale[128];
static pthread_once_t locale_once = PTHREAD_ONCE_INIT;

static void language_tag(CFStringRef id, char *out, size_t cap) {
    CFStringRef tag = CFLocaleCreateCanonicalLanguageIdentifierFromString(NULL, id);
    if (!tag) return;
    if (CFStringGetCString(tag, out, cap, kCFStringEncodingUTF8)) {
        // CF may retain ICU locale keywords after the language identifier.
        char *keywords = strchr(out, '@');
        if (keywords) *keywords = 0;
    } else out[0] = 0;
    CFRelease(tag);
}

static void read_locale(void) {
    CFArrayRef languages = CFLocaleCopyPreferredLanguages();
    if (languages) {
        if (CFArrayGetCount(languages))
            language_tag(CFArrayGetValueAtIndex(languages, 0), ui_language, sizeof ui_language);
        CFRelease(languages);
    }
    CFLocaleRef locale = CFLocaleCopyCurrent();
    if (locale) {
        language_tag(CFLocaleGetIdentifier(locale), user_locale, sizeof user_locale);
        CFRelease(locale);
    }
}

const char *plat_ui_language(void) {
    pthread_once(&locale_once, read_locale);
    return ui_language;
}

const char *plat_user_locale(void) {
    pthread_once(&locale_once, read_locale);
    return user_locale;
}
