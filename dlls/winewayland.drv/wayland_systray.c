/*
 * Wayland driver system tray support, exported as StatusNotifierItems
 *
 * Copyright 2026 Hyunsik Lee
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "shellapi.h"

#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define ITEM_IFACE    "org.kde.StatusNotifierItem"
#define MENU_IFACE    "com.canonical.dbusmenu"
#define PROPS_IFACE   "org.freedesktop.DBus.Properties"
#define PATH_PREFIX   "/org/winehq/wine/tray/"

enum menu_id
{
    MENU_ROOT = 0,
    MENU_OPEN = 1,
    MENU_APP_MENU = 2,
    MENU_SEPARATOR = 3,
    MENU_QUIT = 4,
};

struct tray_icon
{
    struct list    entry;
    unsigned int   serial;           /* used in the object path */
    BOOL           alive;            /* FALSE once deleted, the object is kept as Passive */
    HWND           owner;            /* the HWND passed in to the Shell_NotifyIcon call */
    UINT           id;               /* the unique id given by the app */
    UINT           callback_message;
    UINT           version;
    DWORD          state;
    char           tip[512];         /* UTF-8 tooltip */
    int            width, height;
    unsigned char *pixmap;           /* ARGB32, network byte order */
};

static pthread_mutex_t systray_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t systray_cond = PTHREAD_COND_INITIALIZER;
static DBusConnection *connection;
static BOOL connection_failed;
static struct list icon_list = LIST_INIT(icon_list);
static unsigned int icon_serial;

static BOOL delete_icon(struct tray_icon *icon);

static BOOL use_korean(void)
{
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = getenv("LANG");
    return lang && !strncmp(lang, "ko", 2);
}

static const char *menu_label(int id)
{
    BOOL ko = use_korean();
    switch (id)
    {
    case MENU_OPEN:     return ko ? "열기" : "Open";
    case MENU_APP_MENU: return ko ? "프로그램 메뉴" : "Application Menu";
    case MENU_QUIT:     return ko ? "종료" : "Quit";
    }
    return "";
}

static void wchar_to_utf8(const WCHAR *src, char *dst, size_t size)
{
    size_t pos = 0;

    while (*src && pos + 4 < size)
    {
        unsigned int ch = *src++;
        if (ch >= 0xd800 && ch < 0xdc00 && *src >= 0xdc00 && *src < 0xe000)
            ch = 0x10000 + ((ch - 0xd800) << 10) + (*src++ - 0xdc00);
        if (ch < 0x80) dst[pos++] = ch;
        else if (ch < 0x800)
        {
            dst[pos++] = 0xc0 | (ch >> 6);
            dst[pos++] = 0x80 | (ch & 0x3f);
        }
        else if (ch < 0x10000)
        {
            if (ch >= 0xd800 && ch < 0xe000) continue; /* unpaired surrogate */
            dst[pos++] = 0xe0 | (ch >> 12);
            dst[pos++] = 0x80 | ((ch >> 6) & 0x3f);
            dst[pos++] = 0x80 | (ch & 0x3f);
        }
        else
        {
            dst[pos++] = 0xf0 | (ch >> 18);
            dst[pos++] = 0x80 | ((ch >> 12) & 0x3f);
            dst[pos++] = 0x80 | ((ch >> 6) & 0x3f);
            dst[pos++] = 0x80 | (ch & 0x3f);
        }
    }
    dst[pos] = 0;
}

/* Convert an icon to an unpremultiplied ARGB32 big-endian pixmap. */
static void set_icon_pixmap(struct tray_icon *icon, HICON hicon)
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    unsigned char *mask_bits = NULL, *out;
    unsigned int *bits = NULL;
    ICONINFO ii;
    BITMAP bm;
    BOOL has_alpha = FALSE;
    HDC hdc;
    int i, j;

    free(icon->pixmap);
    icon->pixmap = NULL;
    icon->width = icon->height = 0;

    if (!hicon || !NtUserGetIconInfo(hicon, &ii, NULL, NULL, NULL, 0)) return;
    hdc = NtGdiCreateCompatibleDC(0);

    if (!ii.hbmColor || !NtGdiExtGetObjectW(ii.hbmColor, sizeof(bm), &bm)) goto done;
    if (!(bits = malloc(bm.bmWidth * bm.bmHeight * 4))) goto done;

    memset(info, 0, sizeof(info->bmiHeader));
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = bm.bmWidth;
    info->bmiHeader.biHeight = -bm.bmHeight;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = bm.bmWidth * bm.bmHeight * 4;
    if (!NtGdiGetDIBitsInternal(hdc, ii.hbmColor, 0, bm.bmHeight, bits, info, DIB_RGB_COLORS, 0, 0))
        goto done;

    for (i = 0; i < bm.bmWidth * bm.bmHeight; i++)
        if ((has_alpha = (bits[i] & 0xff000000) != 0)) break;

    if (!has_alpha && ii.hbmMask)
    {
        unsigned int width_bytes = (bm.bmWidth + 31) / 32 * 4;
        info->bmiHeader.biBitCount = 1;
        info->bmiHeader.biSizeImage = width_bytes * bm.bmHeight;
        if (!(mask_bits = malloc(info->bmiHeader.biSizeImage))) goto done;
        if (!NtGdiGetDIBitsInternal(hdc, ii.hbmMask, 0, bm.bmHeight, mask_bits, info, DIB_RGB_COLORS, 0, 0))
            goto done;
        for (i = 0; i < bm.bmHeight; i++)
            for (j = 0; j < bm.bmWidth; j++)
                if (!((mask_bits[i * width_bytes + j / 8] << (j % 8)) & 0x80))
                    bits[i * bm.bmWidth + j] |= 0xff000000;
    }

    if (!(icon->pixmap = malloc(bm.bmWidth * bm.bmHeight * 4))) goto done;
    for (i = 0, out = icon->pixmap; i < bm.bmWidth * bm.bmHeight; i++, out += 4)
    {
        unsigned int p = bits[i];
        out[0] = p >> 24;
        out[1] = p >> 16;
        out[2] = p >> 8;
        out[3] = p;
    }
    icon->width = bm.bmWidth;
    icon->height = bm.bmHeight;

done:
    free(mask_bits);
    free(bits);
    if (ii.hbmColor) NtGdiDeleteObjectApp(ii.hbmColor);
    if (ii.hbmMask) NtGdiDeleteObjectApp(ii.hbmMask);
    NtGdiDeleteObjectApp(hdc);
}

static void icon_path(const struct tray_icon *icon, char *buf, size_t size)
{
    snprintf(buf, size, PATH_PREFIX "%u", icon->serial);
}

static void menu_path(const struct tray_icon *icon, char *buf, size_t size)
{
    snprintf(buf, size, PATH_PREFIX "%u/menu", icon->serial);
}

/* must be called with systray_mutex held */
static struct tray_icon *icon_from_path(const char *path, BOOL *is_menu)
{
    struct tray_icon *icon;
    unsigned int serial;
    int len = 0;

    if (!path || strncmp(path, PATH_PREFIX, strlen(PATH_PREFIX))) return NULL;
    path += strlen(PATH_PREFIX);
    if (sscanf(path, "%u%n", &serial, &len) != 1) return NULL;
    if (!path[len]) *is_menu = FALSE;
    else if (!strcmp(path + len, "/menu")) *is_menu = TRUE;
    else return NULL;

    LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
        if (icon->serial == serial) return icon;
    return NULL;
}

/* must be called with systray_mutex held */
static struct tray_icon *get_icon(HWND owner, UINT id)
{
    struct tray_icon *icon;

    LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
        if (icon->alive && icon->id == id && icon->owner == owner) return icon;
    return NULL;
}

static BOOL icon_visible(const struct tray_icon *icon)
{
    return icon->alive && !(icon->state & NIS_HIDDEN);
}

/**********************************************************************
 *          D-Bus helpers
 */

static void append_variant_string(DBusMessageIter *iter, const char *str)
{
    DBusMessageIter var;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &str);
    dbus_message_iter_close_container(iter, &var);
}

static void append_pixmaps(DBusMessageIter *iter, const struct tray_icon *icon)
{
    DBusMessageIter array, strct, bytes;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "(iiay)", &array);
    if (icon->pixmap)
    {
        dbus_int32_t w = icon->width, h = icon->height;
        const unsigned char *data = icon->pixmap;
        dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &strct);
        dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &w);
        dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &h);
        dbus_message_iter_open_container(&strct, DBUS_TYPE_ARRAY, "y", &bytes);
        dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data, w * h * 4);
        dbus_message_iter_close_container(&strct, &bytes);
        dbus_message_iter_close_container(&array, &strct);
    }
    dbus_message_iter_close_container(iter, &array);
}

static const char *item_properties[] =
{
    "Category", "Id", "Title", "Status", "WindowId", "IconName", "IconPixmap",
    "OverlayIconName", "AttentionIconName", "ToolTip", "ItemIsMenu", "Menu",
};

/* Append the value (as a variant) of a StatusNotifierItem property. */
static BOOL append_item_property(DBusMessageIter *iter, const struct tray_icon *icon, const char *name)
{
    DBusMessageIter var, strct;
    const char *empty = "";

    if (!strcmp(name, "Category")) append_variant_string(iter, "ApplicationStatus");
    else if (!strcmp(name, "Id"))
    {
        char id[64];
        snprintf(id, sizeof(id), "wine_tray_%u", icon->serial);
        append_variant_string(iter, id);
    }
    else if (!strcmp(name, "Title")) append_variant_string(iter, icon->tip);
    else if (!strcmp(name, "Status")) append_variant_string(iter, icon_visible(icon) ? "Active" : "Passive");
    else if (!strcmp(name, "WindowId"))
    {
        dbus_int32_t zero = 0;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &zero);
        dbus_message_iter_close_container(iter, &var);
    }
    else if (!strcmp(name, "IconName") || !strcmp(name, "OverlayIconName") ||
             !strcmp(name, "AttentionIconName"))
        append_variant_string(iter, "");
    else if (!strcmp(name, "IconPixmap"))
    {
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a(iiay)", &var);
        append_pixmaps(&var, icon);
        dbus_message_iter_close_container(iter, &var);
    }
    else if (!strcmp(name, "ToolTip"))
    {
        const char *tip = icon->tip;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", &var);
        dbus_message_iter_open_container(&var, DBUS_TYPE_STRUCT, NULL, &strct);
        dbus_message_iter_append_basic(&strct, DBUS_TYPE_STRING, &empty);
        append_pixmaps(&strct, icon);
        dbus_message_iter_append_basic(&strct, DBUS_TYPE_STRING, &tip);
        dbus_message_iter_append_basic(&strct, DBUS_TYPE_STRING, &empty);
        dbus_message_iter_close_container(&var, &strct);
        dbus_message_iter_close_container(iter, &var);
    }
    else if (!strcmp(name, "ItemIsMenu"))
    {
        dbus_bool_t no = FALSE;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &no);
        dbus_message_iter_close_container(iter, &var);
    }
    else if (!strcmp(name, "Menu"))
    {
        char path[64];
        const char *p = path;
        menu_path(icon, path, sizeof(path));
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "o", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_OBJECT_PATH, &p);
        dbus_message_iter_close_container(iter, &var);
    }
    else return FALSE;
    return TRUE;
}

static const char *menu_properties[] = { "Version", "TextDirection", "Status", "IconThemePath" };

static BOOL append_menu_property(DBusMessageIter *iter, const char *name)
{
    DBusMessageIter var, array;

    if (!strcmp(name, "Version"))
    {
        dbus_uint32_t version = 3;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "u", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &version);
        dbus_message_iter_close_container(iter, &var);
    }
    else if (!strcmp(name, "TextDirection")) append_variant_string(iter, "ltr");
    else if (!strcmp(name, "Status")) append_variant_string(iter, "normal");
    else if (!strcmp(name, "IconThemePath"))
    {
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &var);
        dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &array);
        dbus_message_iter_close_container(&var, &array);
        dbus_message_iter_close_container(iter, &var);
    }
    else return FALSE;
    return TRUE;
}

static void append_dict_entry_string(DBusMessageIter *dict, const char *key, const char *value)
{
    DBusMessageIter entry;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    append_variant_string(&entry, value);
    dbus_message_iter_close_container(dict, &entry);
}

static void append_menu_item_props(DBusMessageIter *iter, int id)
{
    DBusMessageIter dict;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (id == MENU_ROOT) append_dict_entry_string(&dict, "children-display", "submenu");
    else if (id == MENU_SEPARATOR) append_dict_entry_string(&dict, "type", "separator");
    else append_dict_entry_string(&dict, "label", menu_label(id));
    dbus_message_iter_close_container(iter, &dict);
}

static const int menu_items[] = { MENU_OPEN, MENU_APP_MENU, MENU_SEPARATOR, MENU_QUIT };

/* layout of a menu item: (ia{sv}av) */
static void append_menu_layout(DBusMessageIter *iter, int id, BOOL children)
{
    DBusMessageIter strct, array, var;
    dbus_int32_t item_id = id;
    int i;

    dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &strct);
    dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &item_id);
    append_menu_item_props(&strct, id);
    dbus_message_iter_open_container(&strct, DBUS_TYPE_ARRAY, "v", &array);
    if (id == MENU_ROOT && children)
    {
        for (i = 0; i < ARRAY_SIZE(menu_items); i++)
        {
            dbus_message_iter_open_container(&array, DBUS_TYPE_VARIANT, "(ia{sv}av)", &var);
            append_menu_layout(&var, menu_items[i], FALSE);
            dbus_message_iter_close_container(&array, &var);
        }
    }
    dbus_message_iter_close_container(&strct, &array);
    dbus_message_iter_close_container(iter, &strct);
}

static void emit_signal(const char *path, const char *iface, const char *name, const char *str_arg)
{
    DBusMessage *msg;

    if (!(msg = dbus_message_new_signal(path, iface, name))) return;
    if (str_arg) dbus_message_append_args(msg, DBUS_TYPE_STRING, &str_arg, DBUS_TYPE_INVALID);
    dbus_connection_send(connection, msg, NULL);
    dbus_message_unref(msg);
}

static void register_icon(const struct tray_icon *icon)
{
    DBusMessage *msg;
    char path[64];
    const char *p = path;

    icon_path(icon, path, sizeof(path));
    TRACE("registering %s\n", path);
    if (!(msg = dbus_message_new_method_call(WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                                             "RegisterStatusNotifierItem")))
        return;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(connection, msg, NULL);
    dbus_message_unref(msg);
}

/**********************************************************************
 *          Actions forwarded to the owner of the icon
 */

struct icon_action
{
    HWND owner;
    UINT id;
    UINT callback_message;
    UINT version;
};

static void notify_owner(const struct icon_action *action, UINT msg)
{
    WPARAM wp = action->id;
    LPARAM lp = msg;
    POINT pt = {0};

    if (!action->callback_message) return;
    NtUserGetCursorPos(&pt);
    if (action->version >= NOTIFYICON_VERSION_4)
    {
        wp = MAKEWPARAM(pt.x, pt.y);
        lp = MAKELPARAM(msg, action->id);
    }

    TRACE("posting msg 0x%04x to hwnd %p id 0x%x\n", msg, action->owner, action->id);
    NtUserMessageCall(action->owner, action->callback_message, wp, lp, 0,
                      NtUserSendNotifyMessage, FALSE);
}

static void activate_icon(const struct icon_action *action)
{
    notify_owner(action, WM_LBUTTONDOWN);
    notify_owner(action, WM_LBUTTONUP);
    if (action->version) notify_owner(action, NIN_SELECT);
    notify_owner(action, WM_LBUTTONDBLCLK);
    notify_owner(action, WM_LBUTTONUP);
}

static void show_app_menu(const struct icon_action *action)
{
    notify_owner(action, WM_RBUTTONDOWN);
    notify_owner(action, WM_RBUTTONUP);
    if (action->version) notify_owner(action, WM_CONTEXTMENU);
}

static void quit_owner(const struct icon_action *action)
{
    OBJECT_ATTRIBUTES attr;
    CLIENT_ID cid;
    HANDLE process;
    DWORD pid = 0;

    if (!NtUserGetWindowThread(action->owner, &pid) || !pid) return;
    TRACE("terminating process %04x owning hwnd %p\n", (int)pid, action->owner);

    InitializeObjectAttributes(&attr, NULL, 0, NULL, NULL);
    cid.UniqueProcess = ULongToHandle(pid);
    cid.UniqueThread = 0;
    if (!NtOpenProcess(&process, PROCESS_TERMINATE, &attr, &cid))
    {
        NtTerminateProcess(process, 0);
        NtClose(process);
    }
}

static void menu_event(const struct icon_action *action, int id)
{
    switch (id)
    {
    case MENU_OPEN:     activate_icon(action); break;
    case MENU_APP_MENU: show_app_menu(action); break;
    case MENU_QUIT:     quit_owner(action); break;
    }
}

/**********************************************************************
 *          D-Bus message handling
 */

static DBusMessage *handle_properties(DBusMessage *msg, const struct tray_icon *icon, BOOL is_menu)
{
    DBusMessage *reply = NULL;
    DBusMessageIter iter, dict, entry;
    const char *iface, *name;
    int i;

    if (dbus_message_is_method_call(msg, PROPS_IFACE, "Get"))
    {
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_INVALID))
            return NULL;
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        if (is_menu ? append_menu_property(&iter, name) : append_item_property(&iter, icon, name))
            return reply;
        dbus_message_unref(reply);
        return dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, name);
    }
    if (dbus_message_is_method_call(msg, PROPS_IFACE, "GetAll"))
    {
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        for (i = 0; i < (is_menu ? ARRAY_SIZE(menu_properties) : ARRAY_SIZE(item_properties)); i++)
        {
            name = is_menu ? menu_properties[i] : item_properties[i];
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
            if (is_menu) append_menu_property(&entry, name);
            else append_item_property(&entry, icon, name);
            dbus_message_iter_close_container(&dict, &entry);
        }
        dbus_message_iter_close_container(&iter, &dict);
        return reply;
    }
    return NULL;
}

static DBusMessage *handle_menu(DBusMessage *msg, int *event_id)
{
    DBusMessage *reply = NULL;
    DBusMessageIter iter, array, strct, sub;
    dbus_int32_t id, depth;
    const char *event;

    if (dbus_message_is_method_call(msg, MENU_IFACE, "GetLayout"))
    {
        dbus_uint32_t revision = 1;
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INT32, &id, DBUS_TYPE_INT32, &depth,
                                   DBUS_TYPE_INVALID))
            return NULL;
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &revision);
        append_menu_layout(&iter, id, depth != 0);
        return reply;
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "GetGroupProperties"))
    {
        DBusMessageIter ids;
        int i, count = 0;
        dbus_int32_t requested[16];

        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY)
        {
            dbus_message_iter_recurse(&iter, &ids);
            while (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_INT32 && count < ARRAY_SIZE(requested))
            {
                dbus_message_iter_get_basic(&ids, &requested[count++]);
                dbus_message_iter_next(&ids);
            }
        }
        if (!count)
        {
            requested[count++] = MENU_ROOT;
            for (i = 0; i < ARRAY_SIZE(menu_items); i++) requested[count++] = menu_items[i];
        }
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ia{sv})", &array);
        for (i = 0; i < count; i++)
        {
            dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &strct);
            dbus_message_iter_append_basic(&strct, DBUS_TYPE_INT32, &requested[i]);
            append_menu_item_props(&strct, requested[i]);
            dbus_message_iter_close_container(&array, &strct);
        }
        dbus_message_iter_close_container(&iter, &array);
        return reply;
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "GetProperty"))
    {
        const char *name;
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INT32, &id, DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_INVALID))
            return NULL;
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        append_variant_string(&iter, strcmp(name, "label") ? "" : menu_label(id));
        return reply;
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "Event"))
    {
        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT32) return NULL;
        dbus_message_iter_get_basic(&iter, &id);
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return NULL;
        dbus_message_iter_get_basic(&iter, &event);
        if (!strcmp(event, "clicked")) *event_id = id;
        return dbus_message_new_method_return(msg);
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "EventGroup"))
    {
        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY)
        {
            dbus_message_iter_recurse(&iter, &array);
            while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT)
            {
                dbus_message_iter_recurse(&array, &strct);
                dbus_message_iter_get_basic(&strct, &id);
                dbus_message_iter_next(&strct);
                dbus_message_iter_get_basic(&strct, &event);
                if (!strcmp(event, "clicked")) *event_id = id;
                dbus_message_iter_next(&array);
            }
        }
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "i", &sub);
        dbus_message_iter_close_container(&iter, &sub);
        return reply;
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "AboutToShow"))
    {
        dbus_bool_t need_update = FALSE;
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &need_update, DBUS_TYPE_INVALID);
        return reply;
    }
    if (dbus_message_is_method_call(msg, MENU_IFACE, "AboutToShowGroup"))
    {
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "i", &sub);
        dbus_message_iter_close_container(&iter, &sub);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "i", &sub);
        dbus_message_iter_close_container(&iter, &sub);
        return reply;
    }
    return NULL;
}

static DBusHandlerResult filter_func(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct icon_action action = {0};
    struct tray_icon *icon;
    DBusMessage *reply = NULL;
    BOOL is_menu = FALSE, handled = FALSE;
    enum { ACT_NONE, ACT_ACTIVATE, ACT_SECONDARY, ACT_APP_MENU, ACT_MENU } act = ACT_NONE;
    int event_id = -1;

    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged"))
    {
        const char *name, *old_owner, *new_owner;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
            !strcmp(name, WATCHER_NAME) && *new_owner)
        {
            TRACE("status notifier watcher appeared, registering icons\n");
            pthread_mutex_lock(&systray_mutex);
            LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
                if (icon->alive) register_icon(icon);
            pthread_mutex_unlock(&systray_mutex);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    pthread_mutex_lock(&systray_mutex);
    if ((icon = icon_from_path(dbus_message_get_path(msg), &is_menu)))
    {
        action.owner = icon->owner;
        action.id = icon->id;
        action.callback_message = icon->callback_message;
        action.version = icon->version;

        if ((reply = handle_properties(msg, icon, is_menu))) handled = TRUE;
        else if (is_menu)
        {
            if ((reply = handle_menu(msg, &event_id))) handled = TRUE;
            if (event_id != -1) act = ACT_MENU;
        }
        else
        {
            if (dbus_message_is_method_call(msg, ITEM_IFACE, "Activate")) act = ACT_ACTIVATE;
            else if (dbus_message_is_method_call(msg, ITEM_IFACE, "SecondaryActivate")) act = ACT_SECONDARY;
            else if (dbus_message_is_method_call(msg, ITEM_IFACE, "ContextMenu")) act = ACT_APP_MENU;
            if (act != ACT_NONE ||
                dbus_message_is_method_call(msg, ITEM_IFACE, "Scroll") ||
                dbus_message_is_method_call(msg, ITEM_IFACE, "ProvideXdgActivationToken"))
            {
                reply = dbus_message_new_method_return(msg);
                handled = TRUE;
            }
        }
        if (!icon->alive) act = ACT_NONE;
    }
    pthread_mutex_unlock(&systray_mutex);

    if (reply)
    {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
    }

    switch (act)
    {
    case ACT_NONE: break;
    case ACT_ACTIVATE: activate_icon(&action); break;
    case ACT_SECONDARY:
        notify_owner(&action, WM_MBUTTONDOWN);
        notify_owner(&action, WM_MBUTTONUP);
        break;
    case ACT_APP_MENU: show_app_menu(&action); break;
    case ACT_MENU: menu_event(&action, event_id); break;
    }

    return handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* must be called with systray_mutex held */
static BOOL init_connection(void)
{
    DBusError err;

    if (connection) return TRUE;
    if (connection_failed) return FALSE;

    dbus_threads_init_default();
    dbus_error_init(&err);
    if (!(connection = dbus_bus_get_private(DBUS_BUS_SESSION, &err)))
    {
        WARN("failed to connect to the session bus: %s\n", err.message);
        dbus_error_free(&err);
        connection_failed = TRUE;
        return FALSE;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    dbus_connection_add_filter(connection, filter_func, NULL, NULL);
    dbus_bus_add_match(connection, "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='"
                       DBUS_INTERFACE_DBUS "',member='NameOwnerChanged',arg0='" WATCHER_NAME "'", NULL);
    pthread_cond_broadcast(&systray_cond);
    return TRUE;
}

/**********************************************************************
 *          waylanddrv_unix_systray_run
 *
 * Dispatch D-Bus messages for the tray icons. Runs in a dedicated thread.
 */
NTSTATUS waylanddrv_unix_systray_run(void *arg)
{
    unsigned int iterations = 0;

    pthread_mutex_lock(&systray_mutex);
    while (!connection) pthread_cond_wait(&systray_cond, &systray_mutex);
    pthread_mutex_unlock(&systray_mutex);

    while (dbus_connection_read_write_dispatch(connection, 100))
    {
        struct tray_icon *icon;
        DWORD pid;

        if (++iterations % 20) continue;

        /* Remove the icons of windows which were destroyed without
         * deleting their icon, e.g. when their process was killed. */
        pthread_mutex_lock(&systray_mutex);
        LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
        {
            if (icon->alive && !NtUserGetWindowThread(icon->owner, &pid))
            {
                WARN("window %p was destroyed, removing icon 0x%x\n", icon->owner, icon->id);
                delete_icon(icon);
            }
        }
        pthread_mutex_unlock(&systray_mutex);
    }
    WARN("lost connection to the session bus\n");
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Icon management
 */

/* must be called with systray_mutex held */
static BOOL modify_icon(struct tray_icon *icon, NOTIFYICONDATAW *nid, BOOL is_new)
{
    BOOL was_visible = !is_new && icon_visible(icon);
    char path[64];

    TRACE("hwnd %p id 0x%x flags %x\n", nid->hWnd, nid->uID, nid->uFlags);

    icon_path(icon, path, sizeof(path));

    if (nid->uFlags & NIF_STATE)
        icon->state = (icon->state & ~nid->dwStateMask) | (nid->dwState & nid->dwStateMask);
    if (nid->uFlags & NIF_MESSAGE)
        icon->callback_message = nid->uCallbackMessage;
    if (nid->uFlags & NIF_ICON)
    {
        set_icon_pixmap(icon, nid->hIcon);
        if (!is_new) emit_signal(path, ITEM_IFACE, "NewIcon", NULL);
    }
    if (nid->uFlags & NIF_TIP)
    {
        wchar_to_utf8(nid->szTip, icon->tip, sizeof(icon->tip));
        if (!is_new)
        {
            emit_signal(path, ITEM_IFACE, "NewTitle", NULL);
            emit_signal(path, ITEM_IFACE, "NewToolTip", NULL);
        }
    }

    if (is_new) register_icon(icon);
    else if (was_visible != icon_visible(icon))
        emit_signal(path, ITEM_IFACE, "NewStatus", icon_visible(icon) ? "Active" : "Passive");

    dbus_connection_flush(connection);
    return TRUE;
}

/* must be called with systray_mutex held */
static BOOL add_icon(NOTIFYICONDATAW *nid)
{
    struct tray_icon *icon;
    BOOL reused = FALSE;

    TRACE("hwnd %p id 0x%x\n", nid->hWnd, nid->uID);

    if (get_icon(nid->hWnd, nid->uID))
    {
        WARN("duplicate tray icon add, buggy app?\n");
        return FALSE;
    }

    /* Objects of deleted icons are kept around since the watcher only
     * forgets about them when our bus connection goes away, reuse them. */
    LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
        if (!icon->alive) { reused = TRUE; break; }

    if (!reused)
    {
        if (!(icon = calloc(1, sizeof(*icon)))) return FALSE;
        icon->serial = ++icon_serial;
        list_add_tail(&icon_list, &icon->entry);
    }

    icon->alive = TRUE;
    icon->owner = nid->hWnd;
    icon->id = nid->uID;
    icon->callback_message = 0;
    icon->version = 0;
    icon->state = 0;
    icon->tip[0] = 0;
    if (reused) set_icon_pixmap(icon, NULL);

    modify_icon(icon, nid, !reused);
    if (reused)
    {
        char path[64];
        icon_path(icon, path, sizeof(path));
        emit_signal(path, ITEM_IFACE, "NewIcon", NULL);
        emit_signal(path, ITEM_IFACE, "NewTitle", NULL);
        emit_signal(path, ITEM_IFACE, "NewToolTip", NULL);
        emit_signal(path, ITEM_IFACE, "NewStatus", icon_visible(icon) ? "Active" : "Passive");
        dbus_connection_flush(connection);
    }
    return TRUE;
}

/* must be called with systray_mutex held */
static BOOL delete_icon(struct tray_icon *icon)
{
    char path[64];

    TRACE("hwnd %p id 0x%x\n", icon->owner, icon->id);

    icon->alive = FALSE;
    icon->owner = 0;
    icon_path(icon, path, sizeof(path));
    emit_signal(path, ITEM_IFACE, "NewStatus", "Passive");
    dbus_connection_flush(connection);
    return TRUE;
}

/**********************************************************************
 *          WAYLAND_NotifyIcon
 */
LRESULT WAYLAND_NotifyIcon(HWND hwnd, UINT msg, NOTIFYICONDATAW *data)
{
    struct tray_icon *icon;
    LRESULT ret = FALSE;

    pthread_mutex_lock(&systray_mutex);

    if (!init_connection())
    {
        pthread_mutex_unlock(&systray_mutex);
        return -1;
    }

    switch (msg)
    {
    case NIM_ADD:
        ret = add_icon(data);
        break;
    case NIM_DELETE:
        if ((icon = get_icon(data->hWnd, data->uID))) ret = delete_icon(icon);
        break;
    case NIM_MODIFY:
        if ((icon = get_icon(data->hWnd, data->uID))) ret = modify_icon(icon, data, FALSE);
        break;
    case NIM_SETVERSION:
        if ((icon = get_icon(data->hWnd, data->uID)))
        {
            icon->version = data->uVersion;
            ret = TRUE;
        }
        break;
    default:
        ERR("unexpected NotifyIcon call %u\n", msg);
        ret = -1;
        break;
    }

    pthread_mutex_unlock(&systray_mutex);
    return ret;
}

/**********************************************************************
 *          WAYLAND_CleanupIcons
 */
void WAYLAND_CleanupIcons(HWND hwnd)
{
    struct tray_icon *icon;

    pthread_mutex_lock(&systray_mutex);
    if (connection)
    {
        LIST_FOR_EACH_ENTRY(icon, &icon_list, struct tray_icon, entry)
            if (icon->alive && icon->owner == hwnd) delete_icon(icon);
    }
    pthread_mutex_unlock(&systray_mutex);
}
