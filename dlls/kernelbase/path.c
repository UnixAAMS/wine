/*
 * Copyright 2018 Nikolay Sivov
 * Copyright 2018 Zhiyi Zhang
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "pathcch.h"
#include "strsafe.h"
#include "shlwapi.h"
#include "wininet.h"
#include "intshcut.h"
#include "winternl.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(path);

static const char hexDigits[] = "0123456789ABCDEF";

struct parsed_url
{
    const WCHAR *scheme;   /* [out] start of scheme                     */
    DWORD scheme_len;      /* [out] size of scheme (until colon)        */
    const WCHAR *username; /* [out] start of Username                   */
    DWORD username_len;    /* [out] size of Username (until ":" or "@") */
    const WCHAR *password; /* [out] start of Password                   */
    DWORD password_len;    /* [out] size of Password (until "@")        */
    const WCHAR *hostname; /* [out] start of Hostname                   */
    DWORD hostname_len;    /* [out] size of Hostname (until ":" or "/") */
    const WCHAR *port;     /* [out] start of Port                       */
    DWORD port_len;        /* [out] size of Port (until "/" or eos)     */
    const WCHAR *query;    /* [out] start of Query                      */
    DWORD query_len;       /* [out] size of Query (until eos)           */
};

enum url_scan_type
{
    SCHEME,
    HOST,
    PORT,
    USERPASS,
};

static WCHAR *heap_strdupAtoW(const char *str)
{
    WCHAR *ret = NULL;

    if (str)
    {
        DWORD len;

        len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
        ret = heap_alloc(len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, str, -1, ret, len);
    }

    return ret;
}

static SIZE_T strnlenW(const WCHAR *string, SIZE_T maxlen)
{
    SIZE_T i;

    for (i = 0; i < maxlen; i++)
        if (!string[i]) break;
    return i;
}

static BOOL is_prefixed_unc(const WCHAR *string)
{
    static const WCHAR prefixed_unc[] = {'\\', '\\', '?', '\\', 'U', 'N', 'C', '\\'};
    return !strncmpiW(string, prefixed_unc, ARRAY_SIZE(prefixed_unc));
}

static BOOL is_prefixed_disk(const WCHAR *string)
{
    static const WCHAR prefix[] = {'\\', '\\', '?', '\\'};
    return !strncmpW(string, prefix, ARRAY_SIZE(prefix)) && isalphaW(string[4]) && string[5] == ':';
}

static BOOL is_prefixed_volume(const WCHAR *string)
{
    static const WCHAR prefixed_volume[] = {'\\', '\\', '?', '\\', 'V', 'o', 'l', 'u', 'm', 'e'};
    const WCHAR *guid;
    INT i = 0;

    if (strncmpiW(string, prefixed_volume, ARRAY_SIZE(prefixed_volume))) return FALSE;

    guid = string + ARRAY_SIZE(prefixed_volume);

    while (i <= 37)
    {
        switch (i)
        {
        case 0:
            if (guid[i] != '{') return FALSE;
            break;
        case 9:
        case 14:
        case 19:
        case 24:
            if (guid[i] != '-') return FALSE;
            break;
        case 37:
            if (guid[i] != '}') return FALSE;
            break;
        default:
            if (!isalnumW(guid[i])) return FALSE;
            break;
        }
        i++;
    }

    return TRUE;
}

/* Get the next character beyond end of the segment.
   Return TRUE if the last segment ends with a backslash */
static BOOL get_next_segment(const WCHAR *next, const WCHAR **next_segment)
{
    while (*next && *next != '\\') next++;
    if (*next == '\\')
    {
        *next_segment = next + 1;
        return TRUE;
    }
    else
    {
        *next_segment = next;
        return FALSE;
    }
}

/* Find the last character of the root in a path, if there is one, without any segments */
static const WCHAR *get_root_end(const WCHAR *path)
{
    /* Find path root */
    if (is_prefixed_volume(path))
        return path[48] == '\\' ? path + 48 : path + 47;
    else if (is_prefixed_unc(path))
        return path + 7;
    else if (is_prefixed_disk(path))
        return path[6] == '\\' ? path + 6 : path + 5;
    /* \\ */
    else if (path[0] == '\\' && path[1] == '\\')
        return path + 1;
    /* \ */
    else if (path[0] == '\\')
        return path;
    /* X:\ */
    else if (isalphaW(path[0]) && path[1] == ':')
        return path[2] == '\\' ? path + 2 : path + 1;
    else
        return NULL;
}

HRESULT WINAPI PathAllocCanonicalize(const WCHAR *path_in, DWORD flags, WCHAR **path_out)
{
    WCHAR *buffer, *dst;
    const WCHAR *src;
    const WCHAR *root_end;
    SIZE_T buffer_size, length;

    TRACE("%s %#x %p\n", debugstr_w(path_in), flags, path_out);

    if (!path_in || !path_out
        || ((flags & PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS) && (flags & PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS))
        || (flags & (PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS | PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS)
            && !(flags & PATHCCH_ALLOW_LONG_PATHS))
        || ((flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) && (flags & PATHCCH_ALLOW_LONG_PATHS)))
    {
        if (path_out) *path_out = NULL;
        return E_INVALIDARG;
    }

    length = strlenW(path_in);
    if ((length + 1 > MAX_PATH && !(flags & (PATHCCH_ALLOW_LONG_PATHS | PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH)))
        || (length + 1 > PATHCCH_MAX_CCH))
    {
        *path_out = NULL;
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }

    /* PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH implies PATHCCH_DO_NOT_NORMALIZE_SEGMENTS */
    if (flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) flags |= PATHCCH_DO_NOT_NORMALIZE_SEGMENTS;

    /* path length + possible \\?\ addition + possible \ addition + NUL */
    buffer_size = (length + 6) * sizeof(WCHAR);
    buffer = LocalAlloc(LMEM_ZEROINIT, buffer_size);
    if (!buffer)
    {
        *path_out = NULL;
        return E_OUTOFMEMORY;
    }

    src = path_in;
    dst = buffer;

    root_end = get_root_end(path_in);
    if (root_end) root_end = buffer + (root_end - path_in);

    /* Copy path root */
    if (root_end)
    {
        memcpy(dst, src, (root_end - buffer + 1) * sizeof(WCHAR));
        src += root_end - buffer + 1;
        if(PathCchStripPrefix(dst, length + 6) == S_OK)
        {
            /* Fill in \ in X:\ if the \ is missing */
            if(isalphaW(dst[0]) && dst[1] == ':' && dst[2]!= '\\')
            {
                dst[2] = '\\';
                dst[3] = 0;
            }
            dst = buffer + strlenW(buffer);
            root_end = dst;
        }
        else
            dst += root_end - buffer + 1;
    }

    while (*src)
    {
        if (src[0] == '.')
        {
            if (src[1] == '.')
            {
                /* Keep one . after * */
                if (dst > buffer && dst[-1] == '*')
                {
                    *dst++ = *src++;
                    continue;
                }

                /* Keep the . if one of the following is true:
                 * 1. PATHCCH_DO_NOT_NORMALIZE_SEGMENTS
                 * 2. in form of a..b
                 */
                if (dst > buffer
                    && (((flags & PATHCCH_DO_NOT_NORMALIZE_SEGMENTS) && dst[-1] != '\\')
                        || (dst[-1] != '\\' && src[2] != '\\' && src[2])))
                {
                    *dst++ = *src++;
                    *dst++ = *src++;
                    continue;
                }

                /* Remove the \ before .. if the \ is not part of root */
                if (dst > buffer && dst[-1] == '\\' && (!root_end || dst - 1 > root_end))
                {
                    *--dst = '\0';
                    /* Remove characters until a \ is encountered */
                    while (dst > buffer)
                    {
                        if (dst[-1] == '\\')
                        {
                            *--dst = 0;
                            break;
                        }
                        else
                            *--dst = 0;
                    }
                }
                /* Remove the extra \ after .. if the \ before .. wasn't deleted */
                else if (src[2] == '\\')
                    src++;

                src += 2;
            }
            else
            {
                /* Keep the . if one of the following is true:
                 * 1. PATHCCH_DO_NOT_NORMALIZE_SEGMENTS
                 * 2. in form of a.b, which is used in domain names
                 * 3. *.
                 */
                if (dst > buffer
                    && ((flags & PATHCCH_DO_NOT_NORMALIZE_SEGMENTS && dst[-1] != '\\')
                        || (dst[-1] != '\\' && src[1] != '\\' && src[1]) || (dst[-1] == '*')))
                {
                    *dst++ = *src++;
                    continue;
                }

                /* Remove the \ before . if the \ is not part of root */
                if (dst > buffer && dst[-1] == '\\' && (!root_end || dst - 1 > root_end)) dst--;
                /* Remove the extra \ after . if the \ before . wasn't deleted */
                else if (src[1] == '\\')
                    src++;

                src++;
            }

            /* If X:\ is not complete, then complete it */
            if (isalphaW(buffer[0]) && buffer[1] == ':' && buffer[2] != '\\')
            {
                root_end = buffer + 2;
                dst = buffer + 3;
                buffer[2] = '\\';
                /* If next character is \, use the \ to fill in */
                if (src[0] == '\\') src++;
            }
        }
        /* Copy over */
        else
            *dst++ = *src++;
    }
    /* End the path */
    *dst = 0;

    /* If result path is empty, fill in \ */
    if (!*buffer)
    {
        buffer[0] = '\\';
        buffer[1] = 0;
    }

    /* Extend the path if needed */
    length = strlenW(buffer);
    if (((length + 1 > MAX_PATH && isalphaW(buffer[0]) && buffer[1] == ':')
         || (isalphaW(buffer[0]) && buffer[1] == ':' && flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH))
        && !(flags & PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS))
    {
        memmove(buffer + 4, buffer, (length + 1) * sizeof(WCHAR));
        buffer[0] = '\\';
        buffer[1] = '\\';
        buffer[2] = '?';
        buffer[3] = '\\';
    }

    /* Add a trailing backslash to the path if needed */
    if (flags & PATHCCH_ENSURE_TRAILING_SLASH)
        PathCchAddBackslash(buffer, buffer_size);

    *path_out = buffer;
    return S_OK;
}

HRESULT WINAPI PathAllocCombine(const WCHAR *path1, const WCHAR *path2, DWORD flags, WCHAR **out)
{
    SIZE_T combined_length, length2;
    WCHAR *combined_path;
    BOOL from_path2 = FALSE;
    HRESULT hr;

    TRACE("%s %s %#x %p\n", wine_dbgstr_w(path1), wine_dbgstr_w(path2), flags, out);

    if ((!path1 && !path2) || !out)
    {
        if (out) *out = NULL;
        return E_INVALIDARG;
    }

    if (!path1 || !path2) return PathAllocCanonicalize(path1 ? path1 : path2, flags, out);

    /* If path2 is fully qualified, use path2 only */
    if ((isalphaW(path2[0]) && path2[1] == ':') || (path2[0] == '\\' && path2[1] == '\\'))
    {
        path1 = path2;
        path2 = NULL;
        from_path2 = TRUE;
    }

    length2 = path2 ? strlenW(path2) : 0;
    /* path1 length + path2 length + possible backslash + NULL */
    combined_length = strlenW(path1) + length2 + 2;

    combined_path = HeapAlloc(GetProcessHeap(), 0, combined_length * sizeof(WCHAR));
    if (!combined_path)
    {
        *out = NULL;
        return E_OUTOFMEMORY;
    }

    lstrcpyW(combined_path, path1);
    PathCchStripPrefix(combined_path, combined_length);
    if (from_path2) PathCchAddBackslashEx(combined_path, combined_length, NULL, NULL);

    if (path2 && path2[0])
    {
        if (path2[0] == '\\' && path2[1] != '\\')
        {
            PathCchStripToRoot(combined_path, combined_length);
            path2++;
        }

        PathCchAddBackslashEx(combined_path, combined_length, NULL, NULL);
        lstrcatW(combined_path, path2);
    }

    hr = PathAllocCanonicalize(combined_path, flags, out);
    HeapFree(GetProcessHeap(), 0, combined_path);
    return hr;
}

HRESULT WINAPI PathCchAddBackslash(WCHAR *path, SIZE_T size)
{
    return PathCchAddBackslashEx(path, size, NULL, NULL);
}

HRESULT WINAPI PathCchAddBackslashEx(WCHAR *path, SIZE_T size, WCHAR **endptr, SIZE_T *remaining)
{
    BOOL needs_termination;
    SIZE_T length;

    TRACE("%s, %lu, %p, %p\n", debugstr_w(path), size, endptr, remaining);

    length = strlenW(path);
    needs_termination = size && length && path[length - 1] != '\\';

    if (length >= (needs_termination ? size - 1 : size))
    {
        if (endptr) *endptr = NULL;
        if (remaining) *remaining = 0;
        return STRSAFE_E_INSUFFICIENT_BUFFER;
    }

    if (!needs_termination)
    {
        if (endptr) *endptr = path + length;
        if (remaining) *remaining = size - length;
        return S_FALSE;
    }

    path[length++] = '\\';
    path[length] = 0;

    if (endptr) *endptr = path + length;
    if (remaining) *remaining = size - length;

    return S_OK;
}

HRESULT WINAPI PathCchAddExtension(WCHAR *path, SIZE_T size, const WCHAR *extension)
{
    const WCHAR *existing_extension, *next;
    SIZE_T path_length, extension_length, dot_length;
    BOOL has_dot;
    HRESULT hr;

    TRACE("%s %lu %s\n", wine_dbgstr_w(path), size, wine_dbgstr_w(extension));

    if (!path || !size || size > PATHCCH_MAX_CCH || !extension) return E_INVALIDARG;

    next = extension;
    while (*next)
    {
        if ((*next == '.' && next > extension) || *next == ' ' || *next == '\\') return E_INVALIDARG;
        next++;
    }

    has_dot = extension[0] == '.';

    hr = PathCchFindExtension(path, size, &existing_extension);
    if (FAILED(hr)) return hr;
    if (*existing_extension) return S_FALSE;

    path_length = strnlenW(path, size);
    dot_length = has_dot ? 0 : 1;
    extension_length = strlenW(extension);

    if (path_length + dot_length + extension_length + 1 > size) return STRSAFE_E_INSUFFICIENT_BUFFER;

    /* If extension is empty or only dot, return S_OK with path unchanged */
    if (!extension[0] || (extension[0] == '.' && !extension[1])) return S_OK;

    if (!has_dot)
    {
        path[path_length] = '.';
        path_length++;
    }

    strcpyW(path + path_length, extension);
    return S_OK;
}

HRESULT WINAPI PathCchAppend(WCHAR *path1, SIZE_T size, const WCHAR *path2)
{
    TRACE("%s %lu %s\n", wine_dbgstr_w(path1), size, wine_dbgstr_w(path2));

    return PathCchAppendEx(path1, size, path2, PATHCCH_NONE);
}

HRESULT WINAPI PathCchAppendEx(WCHAR *path1, SIZE_T size, const WCHAR *path2, DWORD flags)
{
    HRESULT hr;
    WCHAR *result;

    TRACE("%s %lu %s %#x\n", wine_dbgstr_w(path1), size, wine_dbgstr_w(path2), flags);

    if (!path1 || !size) return E_INVALIDARG;

    /* Create a temporary buffer for result because we need to keep path1 unchanged if error occurs.
     * And PathCchCombineEx writes empty result if there is error so we can't just use path1 as output
     * buffer for PathCchCombineEx */
    result = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
    if (!result) return E_OUTOFMEMORY;

    /* Avoid the single backslash behavior with PathCchCombineEx when appending */
    if (path2 && path2[0] == '\\' && path2[1] != '\\') path2++;

    hr = PathCchCombineEx(result, size, path1, path2, flags);
    if (SUCCEEDED(hr)) memcpy(path1, result, size * sizeof(WCHAR));

    HeapFree(GetProcessHeap(), 0, result);
    return hr;
}

HRESULT WINAPI PathCchCanonicalize(WCHAR *out, SIZE_T size, const WCHAR *in)
{
    TRACE("%p %lu %s\n", out, size, wine_dbgstr_w(in));

    /* Not X:\ and path > MAX_PATH - 4, return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) */
    if (strlenW(in) > MAX_PATH - 4 && !(isalphaW(in[0]) && in[1] == ':' && in[2] == '\\'))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    return PathCchCanonicalizeEx(out, size, in, PATHCCH_NONE);
}

HRESULT WINAPI PathCchCanonicalizeEx(WCHAR *out, SIZE_T size, const WCHAR *in, DWORD flags)
{
    WCHAR *buffer;
    SIZE_T length;
    HRESULT hr;

    TRACE("%p %lu %s %#x\n", out, size, wine_dbgstr_w(in), flags);

    if (!size) return E_INVALIDARG;

    hr = PathAllocCanonicalize(in, flags, &buffer);
    if (FAILED(hr)) return hr;

    length = strlenW(buffer);
    if (size < length + 1)
    {
        /* No root and path > MAX_PATH - 4, return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) */
        if (length > MAX_PATH - 4 && !(in[0] == '\\' || (isalphaW(in[0]) && in[1] == ':' && in[2] == '\\')))
            hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        else
            hr = STRSAFE_E_INSUFFICIENT_BUFFER;
    }

    if (SUCCEEDED(hr))
    {
        memcpy(out, buffer, (length + 1) * sizeof(WCHAR));

        /* Fill a backslash at the end of X: */
        if (isalphaW(out[0]) && out[1] == ':' && !out[2] && size > 3)
        {
            out[2] = '\\';
            out[3] = 0;
        }
    }

    LocalFree(buffer);
    return hr;
}

HRESULT WINAPI PathCchCombine(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2)
{
    TRACE("%p %s %s\n", out, wine_dbgstr_w(path1), wine_dbgstr_w(path2));

    return PathCchCombineEx(out, size, path1, path2, PATHCCH_NONE);
}

HRESULT WINAPI PathCchCombineEx(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2, DWORD flags)
{
    HRESULT hr;
    WCHAR *buffer;
    SIZE_T length;

    TRACE("%p %s %s %#x\n", out, wine_dbgstr_w(path1), wine_dbgstr_w(path2), flags);

    if (!out || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

    hr = PathAllocCombine(path1, path2, flags, &buffer);
    if (FAILED(hr))
    {
        out[0] = 0;
        return hr;
    }

    length = strlenW(buffer);
    if (length + 1 > size)
    {
        out[0] = 0;
        LocalFree(buffer);
        return STRSAFE_E_INSUFFICIENT_BUFFER;
    }
    else
    {
        memcpy(out, buffer, (length + 1) * sizeof(WCHAR));
        LocalFree(buffer);
        return S_OK;
    }
}

HRESULT WINAPI PathCchFindExtension(const WCHAR *path, SIZE_T size, const WCHAR **extension)
{
    const WCHAR *lastpoint = NULL;
    SIZE_T counter = 0;

    TRACE("%s %lu %p\n", wine_dbgstr_w(path), size, extension);

    if (!path || !size || size > PATHCCH_MAX_CCH)
    {
        *extension = NULL;
        return E_INVALIDARG;
    }

    while (*path)
    {
        if (*path == '\\' || *path == ' ')
            lastpoint = NULL;
        else if (*path == '.')
            lastpoint = path;

        path++;
        counter++;
        if (counter == size || counter == PATHCCH_MAX_CCH)
        {
            *extension = NULL;
            return E_INVALIDARG;
        }
    }

    *extension = lastpoint ? lastpoint : path;
    return S_OK;
}

BOOL WINAPI PathCchIsRoot(const WCHAR *path)
{
    const WCHAR *root_end;
    const WCHAR *next;
    BOOL is_unc;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path) return FALSE;

    root_end = get_root_end(path);
    if (!root_end) return FALSE;

    if ((is_unc = is_prefixed_unc(path)) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?'))
    {
        next = root_end + 1;
        /* No extra segments */
        if ((is_unc && !*next) || (!is_unc && !*next)) return TRUE;

        /* Has first segment with an ending backslash but no remaining characters */
        if (get_next_segment(next, &next) && !*next) return FALSE;
        /* Has first segment with no ending backslash */
        else if (!*next)
            return TRUE;
        /* Has first segment with an ending backslash and has remaining characters*/
        else
        {
            next++;
            /* Second segment must have no backslash and no remaining characters */
            return !get_next_segment(next, &next) && !*next;
        }
    }
    else if (*root_end == '\\' && !root_end[1])
        return TRUE;
    else
        return FALSE;
}

HRESULT WINAPI PathCchRemoveBackslash(WCHAR *path, SIZE_T path_size)
{
    WCHAR *path_end;
    SIZE_T free_size;

    TRACE("%s %lu\n", debugstr_w(path), path_size);

    return PathCchRemoveBackslashEx(path, path_size, &path_end, &free_size);
}

HRESULT WINAPI PathCchRemoveBackslashEx(WCHAR *path, SIZE_T path_size, WCHAR **path_end, SIZE_T *free_size)
{
    const WCHAR *root_end;
    SIZE_T path_length;

    TRACE("%s %lu %p %p\n", debugstr_w(path), path_size, path_end, free_size);

    if (!path_size || !path_end || !free_size)
    {
        if (path_end) *path_end = NULL;
        if (free_size) *free_size = 0;
        return E_INVALIDARG;
    }

    path_length = strnlenW(path, path_size);
    if (path_length == path_size && !path[path_length]) return E_INVALIDARG;

    root_end = get_root_end(path);
    if (path_length > 0 && path[path_length - 1] == '\\')
    {
        *path_end = path + path_length - 1;
        *free_size = path_size - path_length + 1;
        /* If the last character is beyond end of root */
        if (!root_end || path + path_length - 1 > root_end)
        {
            path[path_length - 1] = 0;
            return S_OK;
        }
        else
            return S_FALSE;
    }
    else
    {
        *path_end = path + path_length;
        *free_size = path_size - path_length;
        return S_FALSE;
    }
}

HRESULT WINAPI PathCchRemoveExtension(WCHAR *path, SIZE_T size)
{
    const WCHAR *extension;
    WCHAR *next;
    HRESULT hr;

    TRACE("%s %lu\n", wine_dbgstr_w(path), size);

    if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

    hr = PathCchFindExtension(path, size, &extension);
    if (FAILED(hr)) return hr;

    next = path + (extension - path);
    while (next - path < size && *next) *next++ = 0;

    return next == extension ? S_FALSE : S_OK;
}

HRESULT WINAPI PathCchRemoveFileSpec(WCHAR *path, SIZE_T size)
{
    const WCHAR *root_end = NULL;
    SIZE_T length;
    WCHAR *last;

    TRACE("%s %lu\n", wine_dbgstr_w(path), size);

    if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

    if (PathCchIsRoot(path)) return S_FALSE;

    PathCchSkipRoot(path, &root_end);

    /* The backslash at the end of UNC and \\* are not considered part of root in this case */
    if (root_end && root_end > path && root_end[-1] == '\\'
        && (is_prefixed_unc(path) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?')))
        root_end--;

    length = strlenW(path);
    last = path + length - 1;
    while (last >= path && (!root_end || last >= root_end))
    {
        if (last - path >= size) return E_INVALIDARG;

        if (*last == '\\')
        {
            *last-- = 0;
            break;
        }

        *last-- = 0;
    }

    return last != path + length - 1 ? S_OK : S_FALSE;
}

HRESULT WINAPI PathCchRenameExtension(WCHAR *path, SIZE_T size, const WCHAR *extension)
{
    HRESULT hr;

    TRACE("%s %lu %s\n", wine_dbgstr_w(path), size, wine_dbgstr_w(extension));

    hr = PathCchRemoveExtension(path, size);
    if (FAILED(hr)) return hr;

    hr = PathCchAddExtension(path, size, extension);
    return FAILED(hr) ? hr : S_OK;
}

HRESULT WINAPI PathCchSkipRoot(const WCHAR *path, const WCHAR **root_end)
{
    static const WCHAR unc_prefix[] = {'\\', '\\', '?'};

    TRACE("%s %p\n", debugstr_w(path), root_end);

    if (!path || !path[0] || !root_end
        || (!strncmpiW(unc_prefix, path, ARRAY_SIZE(unc_prefix)) && !is_prefixed_volume(path) && !is_prefixed_unc(path)
            && !is_prefixed_disk(path)))
        return E_INVALIDARG;

    *root_end = get_root_end(path);
    if (*root_end)
    {
        (*root_end)++;
        if (is_prefixed_unc(path))
        {
            get_next_segment(*root_end, root_end);
            get_next_segment(*root_end, root_end);
        }
        else if (path[0] == '\\' && path[1] == '\\' && path[2] != '?')
        {
            /* Skip share server */
            get_next_segment(*root_end, root_end);
            /* If mount point is empty, don't skip over mount point */
            if (**root_end != '\\') get_next_segment(*root_end, root_end);
        }
    }

    return *root_end ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI PathCchStripPrefix(WCHAR *path, SIZE_T size)
{
    TRACE("%s %lu\n", wine_dbgstr_w(path), size);

    if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

    if (is_prefixed_unc(path))
    {
        /* \\?\UNC\a -> \\a */
        if (size < strlenW(path + 8) + 3) return E_INVALIDARG;
        strcpyW(path + 2, path + 8);
        return S_OK;
    }
    else if (is_prefixed_disk(path))
    {
        /* \\?\C:\ -> C:\ */
        if (size < strlenW(path + 4) + 1) return E_INVALIDARG;
        strcpyW(path, path + 4);
        return S_OK;
    }
    else
        return S_FALSE;
}

HRESULT WINAPI PathCchStripToRoot(WCHAR *path, SIZE_T size)
{
    const WCHAR *root_end;
    WCHAR *segment_end;
    BOOL is_unc;

    TRACE("%s %lu\n", wine_dbgstr_w(path), size);

    if (!path || !*path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

    /* \\\\?\\UNC\\* and \\\\* have to have at least two extra segments to be striped,
     * e.g. \\\\?\\UNC\\a\\b\\c -> \\\\?\\UNC\\a\\b
     *      \\\\a\\b\\c         -> \\\\a\\b         */
    if ((is_unc = is_prefixed_unc(path)) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?'))
    {
        root_end = is_unc ? path + 8 : path + 3;
        if (!get_next_segment(root_end, &root_end)) return S_FALSE;
        if (!get_next_segment(root_end, &root_end)) return S_FALSE;

        if (root_end - path >= size) return E_INVALIDARG;

        segment_end = path + (root_end - path) - 1;
        *segment_end = 0;
        return S_OK;
    }
    else if (PathCchSkipRoot(path, &root_end) == S_OK)
    {
        if (root_end - path >= size) return E_INVALIDARG;

        segment_end = path + (root_end - path);
        if (!*segment_end) return S_FALSE;

        *segment_end = 0;
        return S_OK;
    }
    else
        return E_INVALIDARG;
}

BOOL WINAPI PathIsUNCEx(const WCHAR *path, const WCHAR **server)
{
    const WCHAR *result = NULL;

    TRACE("%s %p\n", wine_dbgstr_w(path), server);

    if (is_prefixed_unc(path))
        result = path + 8;
    else if (path[0] == '\\' && path[1] == '\\' && path[2] != '?')
        result = path + 2;

    if (server) *server = result;
    return !!result;
}

BOOL WINAPI PathIsUNCA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    return path && (path[0] == '\\') && (path[1] == '\\');
}

BOOL WINAPI PathIsUNCW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    return path && (path[0] == '\\') && (path[1] == '\\');
}

BOOL WINAPI PathIsRelativeA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path || IsDBCSLeadByte(*path))
        return TRUE;

    return !(*path == '\\' || (*path && path[1] == ':'));
}

BOOL WINAPI PathIsRelativeW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return TRUE;

    return !(*path == '\\' || (*path && path[1] == ':'));
}

BOOL WINAPI PathIsUNCServerShareA(const char *path)
{
    BOOL seen_slash = FALSE;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (path && *path++ == '\\' && *path++ == '\\')
    {
        while (*path)
        {
            if (*path == '\\')
            {
                if (seen_slash)
                    return FALSE;
                seen_slash = TRUE;
            }

            path = CharNextA(path);
        }
    }

    return seen_slash;
}

BOOL WINAPI PathIsUNCServerShareW(const WCHAR *path)
{
    BOOL seen_slash = FALSE;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (path && *path++ == '\\' && *path++ == '\\')
    {
        while (*path)
        {
            if (*path == '\\')
            {
                if (seen_slash)
                    return FALSE;
                seen_slash = TRUE;
            }

            path++;
        }
    }

    return seen_slash;
}

BOOL WINAPI PathIsRootA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path)
        return FALSE;

    if (*path == '\\')
    {
        if (!path[1])
            return TRUE; /* \ */
        else if (path[1] == '\\')
        {
            BOOL seen_slash = FALSE;
            path += 2;

            /* Check for UNC root path */
            while (*path)
            {
                if (*path == '\\')
                {
                    if (seen_slash)
                        return FALSE;
                    seen_slash = TRUE;
                }

                path = CharNextA(path);
            }

            return TRUE;
        }
    }
    else if (path[1] == ':' && path[2] == '\\' && path[3] == '\0')
        return TRUE; /* X:\ */

    return FALSE;
}

BOOL WINAPI PathIsRootW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return FALSE;

    if (*path == '\\')
    {
        if (!path[1])
            return TRUE; /* \ */
        else if (path[1] == '\\')
        {
            BOOL seen_slash = FALSE;

            path += 2;
            /* Check for UNC root path */
            while (*path)
            {
                if (*path == '\\')
                {
                    if (seen_slash)
                        return FALSE;
                    seen_slash = TRUE;
                }
                path++;
            }

            return TRUE;
        }
    }
    else if (path[1] == ':' && path[2] == '\\' && path[3] == '\0')
        return TRUE; /* X:\ */

    return FALSE;
}

BOOL WINAPI PathRemoveFileSpecA(char *path)
{
    char *filespec = path;
    BOOL modified = FALSE;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return FALSE;

    /* Skip directory or UNC path */
    if (*path == '\\')
        filespec = ++path;
    if (*path == '\\')
        filespec = ++path;

    while (*path)
    {
        if (*path == '\\')
            filespec = path; /* Skip dir */
        else if (*path == ':')
        {
            filespec = ++path; /* Skip drive */
            if (*path == '\\')
                filespec++;
        }
        if (!(path = CharNextA(path)))
            break;
    }

    if (*filespec)
    {
        *filespec = '\0';
        modified = TRUE;
    }

    return modified;
}

BOOL WINAPI PathRemoveFileSpecW(WCHAR *path)
{
    WCHAR *filespec = path;
    BOOL modified = FALSE;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return FALSE;

    /* Skip directory or UNC path */
    if (*path == '\\')
        filespec = ++path;
    if (*path == '\\')
        filespec = ++path;

    while (*path)
    {
        if (*path == '\\')
            filespec = path; /* Skip dir */
        else if (*path == ':')
        {
            filespec = ++path; /* Skip drive */
            if (*path == '\\')
                filespec++;
        }

        path++;
    }

    if (*filespec)
    {
        *filespec = '\0';
        modified = TRUE;
    }

    return modified;
}

BOOL WINAPI PathStripToRootA(char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return FALSE;

    while (!PathIsRootA(path))
        if (!PathRemoveFileSpecA(path))
            return FALSE;

    return TRUE;
}

BOOL WINAPI PathStripToRootW(WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return FALSE;

    while (!PathIsRootW(path))
        if (!PathRemoveFileSpecW(path))
            return FALSE;

    return TRUE;
}

LPSTR WINAPI PathAddBackslashA(char *path)
{
    unsigned int len;
    char *prev = path;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || (len = strlen(path)) >= MAX_PATH)
        return NULL;

    if (len)
    {
        do
        {
            path = CharNextA(prev);
            if (*path)
            prev = path;
        } while (*path);

        if (*prev != '\\')
        {
            *path++ = '\\';
            *path = '\0';
        }
    }

    return path;
}

LPWSTR WINAPI PathAddBackslashW(WCHAR *path)
{
    unsigned int len;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || (len = strlenW(path)) >= MAX_PATH)
        return NULL;

    if (len)
    {
        path += len;
        if (path[-1] != '\\')
        {
            *path++ = '\\';
            *path = '\0';
        }
    }

    return path;
}

LPSTR WINAPI PathFindExtensionA(const char *path)
{
    const char *lastpoint = NULL;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (path)
    {
        while (*path)
        {
            if (*path == '\\' || *path == ' ')
                lastpoint = NULL;
            else if (*path == '.')
                lastpoint = path;
            path = CharNextA(path);
        }
    }

    return (LPSTR)(lastpoint ? lastpoint : path);
}

LPWSTR WINAPI PathFindExtensionW(const WCHAR *path)
{
    const WCHAR *lastpoint = NULL;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (path)
    {
        while (*path)
        {
            if (*path == '\\' || *path == ' ')
                lastpoint = NULL;
            else if (*path == '.')
                lastpoint = path;
            path++;
        }
    }

    return (LPWSTR)(lastpoint ? lastpoint : path);
}

BOOL WINAPI PathAddExtensionA(char *path, const char *ext)
{
    unsigned int len;

    TRACE("%s, %s\n", wine_dbgstr_a(path), wine_dbgstr_a(ext));

    if (!path || !ext || *(PathFindExtensionA(path)))
        return FALSE;

    len = strlen(path);
    if (len + strlen(ext) >= MAX_PATH)
        return FALSE;

    strcpy(path + len, ext);
    return TRUE;
}

BOOL WINAPI PathAddExtensionW(WCHAR *path, const WCHAR *ext)
{
    unsigned int len;

    TRACE("%s, %s\n", wine_dbgstr_w(path), wine_dbgstr_w(ext));

    if (!path || !ext || *(PathFindExtensionW(path)))
        return FALSE;

    len = strlenW(path);
    if (len + strlenW(ext) >= MAX_PATH)
        return FALSE;

    strcpyW(path + len, ext);
    return TRUE;
}

BOOL WINAPI PathCanonicalizeW(WCHAR *buffer, const WCHAR *path)
{
    const WCHAR *src = path;
    WCHAR *dst = buffer;

    TRACE("%p, %s\n", buffer, wine_dbgstr_w(path));

    if (dst)
        *dst = '\0';

    if (!dst || !path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!*path)
    {
        *buffer++ = '\\';
        *buffer = '\0';
        return TRUE;
    }

    /* Copy path root */
    if (*src == '\\')
    {
        *dst++ = *src++;
    }
    else if (*src && src[1] == ':')
    {
        /* X:\ */
        *dst++ = *src++;
        *dst++ = *src++;
        if (*src == '\\')
            *dst++ = *src++;
    }

    /* Canonicalize the rest of the path */
    while (*src)
    {
        if (*src == '.')
        {
            if (src[1] == '\\' && (src == path || src[-1] == '\\' || src[-1] == ':'))
            {
                src += 2; /* Skip .\ */
            }
            else if (src[1] == '.' && (dst == buffer || dst[-1] == '\\'))
            {
                /* \.. backs up a directory, over the root if it has no \ following X:.
                 * .. is ignored if it would remove a UNC server name or initial \\
                 */
                if (dst != buffer)
                {
                    *dst = '\0'; /* Allow PathIsUNCServerShareA test on lpszBuf */
                    if (dst > buffer + 1 && dst[-1] == '\\' && (dst[-2] != '\\' || dst > buffer + 2))
                    {
                        if (dst[-2] == ':' && (dst > buffer + 3 || dst[-3] == ':'))
                        {
                            dst -= 2;
                            while (dst > buffer && *dst != '\\')
                                dst--;
                            if (*dst == '\\')
                                dst++; /* Reset to last '\' */
                            else
                                dst = buffer; /* Start path again from new root */
                        }
                        else if (dst[-2] != ':' && !PathIsUNCServerShareW(buffer))
                            dst -= 2;
                    }
                    while (dst > buffer && *dst != '\\')
                        dst--;
                    if (dst == buffer)
                    {
                        *dst++ = '\\';
                        src++;
                    }
                }
                src += 2; /* Skip .. in src path */
            }
            else
                *dst++ = *src++;
        }
        else
            *dst++ = *src++;
    }

    /* Append \ to naked drive specs */
    if (dst - buffer == 2 && dst[-1] == ':')
        *dst++ = '\\';
    *dst++ = '\0';
    return TRUE;
}

BOOL WINAPI PathCanonicalizeA(char *buffer, const char *path)
{
    WCHAR pathW[MAX_PATH], bufferW[MAX_PATH];
    BOOL ret;
    int len;

    TRACE("%p, %s\n", buffer, wine_dbgstr_a(path));

    if (buffer)
        *buffer = '\0';

    if (!buffer || !path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    len = MultiByteToWideChar(CP_ACP, 0, path, -1, pathW, ARRAY_SIZE(pathW));
    if (!len)
        return FALSE;

    ret = PathCanonicalizeW(bufferW, pathW);
    WideCharToMultiByte(CP_ACP, 0, bufferW, -1, buffer, MAX_PATH, 0, 0);

    return ret;
}

WCHAR * WINAPI PathCombineW(WCHAR *dst, const WCHAR *dir, const WCHAR *file)
{
    BOOL use_both = FALSE, strip = FALSE;
    WCHAR tmp[MAX_PATH];

    TRACE("%p, %s, %s\n", dst, wine_dbgstr_w(dir), wine_dbgstr_w(file));

    /* Invalid parameters */
    if (!dst)
        return NULL;

    if (!dir && !file)
    {
        dst[0] = 0;
        return NULL;
    }

    if ((!file || !*file) && dir)
    {
        /* Use dir only */
        lstrcpynW(tmp, dir, ARRAY_SIZE(tmp));
    }
    else if (!dir || !*dir || !PathIsRelativeW(file))
    {
        if (!dir || !*dir || *file != '\\' || PathIsUNCW(file))
        {
            /* Use file only */
            lstrcpynW(tmp, file, ARRAY_SIZE(tmp));
        }
        else
        {
            use_both = TRUE;
            strip = TRUE;
        }
    }
    else
        use_both = TRUE;

    if (use_both)
    {
        lstrcpynW(tmp, dir, ARRAY_SIZE(tmp));
        if (strip)
        {
            PathStripToRootW(tmp);
            file++; /* Skip '\' */
        }

        if (!PathAddBackslashW(tmp) || strlenW(tmp) + strlenW(file) >= MAX_PATH)
        {
            dst[0] = 0;
            return NULL;
        }

        strcatW(tmp, file);
    }

    PathCanonicalizeW(dst, tmp);
    return dst;
}

LPSTR WINAPI PathCombineA(char *dst, const char *dir, const char *file)
{
    WCHAR dstW[MAX_PATH], dirW[MAX_PATH], fileW[MAX_PATH];

    TRACE("%p, %s, %s\n", dst, wine_dbgstr_a(dir), wine_dbgstr_a(file));

    /* Invalid parameters */
    if (!dst)
        return NULL;

    dst[0] = 0;

    if (!dir && !file)
        return NULL;

    if (dir && !MultiByteToWideChar(CP_ACP, 0, dir, -1, dirW, ARRAY_SIZE(dirW)))
        return NULL;

    if (file && !MultiByteToWideChar(CP_ACP, 0, file, -1, fileW, ARRAY_SIZE(fileW)))
        return NULL;

    if (PathCombineW(dstW, dir ? dirW : NULL, file ? fileW : NULL))
        if (WideCharToMultiByte(CP_ACP, 0, dstW, -1, dst, MAX_PATH, 0, 0))
            return dst;

    return NULL;
}

BOOL WINAPI PathAppendA(char *path, const char *append)
{
    TRACE("%s, %s\n", wine_dbgstr_a(path), wine_dbgstr_a(append));

    if (path && append)
    {
        if (!PathIsUNCA(append))
            while (*append == '\\')
                append++;

        if (PathCombineA(path, path, append))
            return TRUE;
    }

    return FALSE;
}

BOOL WINAPI PathAppendW(WCHAR *path, const WCHAR *append)
{
    TRACE("%s, %s\n", wine_dbgstr_w(path), wine_dbgstr_w(append));

    if (path && append)
    {
        if (!PathIsUNCW(append))
            while (*append == '\\')
                append++;

        if (PathCombineW(path, path, append))
            return TRUE;
    }

    return FALSE;
}

int WINAPI PathCommonPrefixA(const char *file1, const char *file2, char *path)
{
    const char *iter1 = file1;
    const char *iter2 = file2;
    unsigned int len = 0;

    TRACE("%s, %s, %p.\n", wine_dbgstr_a(file1), wine_dbgstr_a(file2), path);

    if (path)
        *path = '\0';

    if (!file1 || !file2)
        return 0;

    /* Handle roots first */
    if (PathIsUNCA(file1))
    {
        if (!PathIsUNCA(file2))
            return 0;
        iter1 += 2;
        iter2 += 2;
    }
    else if (PathIsUNCA(file2))
        return 0;

    for (;;)
    {
        /* Update len */
        if ((!*iter1 || *iter1 == '\\') && (!*iter2 || *iter2 == '\\'))
            len = iter1 - file1; /* Common to this point */

        if (!*iter1 || (tolower(*iter1) != tolower(*iter2)))
            break; /* Strings differ at this point */

        iter1++;
        iter2++;
    }

    if (len == 2)
        len++; /* Feature/Bug compatible with Win32 */

    if (len && path)
    {
        memcpy(path, file1, len);
        path[len] = '\0';
    }

    return len;
}

int WINAPI PathCommonPrefixW(const WCHAR *file1, const WCHAR *file2, WCHAR *path)
{
    const WCHAR *iter1 = file1;
    const WCHAR *iter2 = file2;
    unsigned int len = 0;

    TRACE("%s, %s, %p\n", wine_dbgstr_w(file1), wine_dbgstr_w(file2), path);

    if (path)
        *path = '\0';

    if (!file1 || !file2)
        return 0;

    /* Handle roots first */
    if (PathIsUNCW(file1))
    {
        if (!PathIsUNCW(file2))
            return 0;
        iter1 += 2;
        iter2 += 2;
    }
    else if (PathIsUNCW(file2))
      return 0;

    for (;;)
    {
        /* Update len */
        if ((!*iter1 || *iter1 == '\\') && (!*iter2 || *iter2 == '\\'))
            len = iter1 - file1; /* Common to this point */

        if (!*iter1 || (tolowerW(*iter1) != tolowerW(*iter2)))
            break; /* Strings differ at this point */

        iter1++;
        iter2++;
    }

    if (len == 2)
        len++; /* Feature/Bug compatible with Win32 */

    if (len && path)
    {
        memcpy(path, file1, len * sizeof(WCHAR));
        path[len] = '\0';
    }

    return len;
}

BOOL WINAPI PathIsPrefixA(const char *prefix, const char *path)
{
    TRACE("%s, %s\n", wine_dbgstr_a(prefix), wine_dbgstr_a(path));

    return prefix && path && PathCommonPrefixA(path, prefix, NULL) == (int)strlen(prefix);
}

BOOL WINAPI PathIsPrefixW(const WCHAR *prefix, const WCHAR *path)
{
    TRACE("%s, %s\n", wine_dbgstr_w(prefix), wine_dbgstr_w(path));

    return prefix && path && PathCommonPrefixW(path, prefix, NULL) == (int)strlenW(prefix);
}

char * WINAPI PathFindFileNameA(const char *path)
{
    const char *last_slash = path;

    TRACE("%s\n", wine_dbgstr_a(path));

    while (path && *path)
    {
        if ((*path == '\\' || *path == '/' || *path == ':') &&
                path[1] && path[1] != '\\' && path[1] != '/')
            last_slash = path + 1;
        path = CharNextA(path);
    }

    return (char *)last_slash;
}

WCHAR * WINAPI PathFindFileNameW(const WCHAR *path)
{
    const WCHAR *last_slash = path;

    TRACE("%s\n", wine_dbgstr_w(path));

    while (path && *path)
    {
        if ((*path == '\\' || *path == '/' || *path == ':') &&
                path[1] && path[1] != '\\' && path[1] != '/')
            last_slash = path + 1;
        path++;
    }

    return (WCHAR *)last_slash;
}

char * WINAPI PathGetArgsA(const char *path)
{
    BOOL seen_quote = FALSE;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return NULL;

    while (*path)
    {
        if (*path == ' ' && !seen_quote)
            return (char *)path + 1;

        if (*path == '"')
            seen_quote = !seen_quote;
        path = CharNextA(path);
    }

    return (char *)path;
}

WCHAR * WINAPI PathGetArgsW(const WCHAR *path)
{
    BOOL seen_quote = FALSE;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return NULL;

    while (*path)
    {
        if (*path == ' ' && !seen_quote)
            return (WCHAR *)path + 1;

        if (*path == '"')
            seen_quote = !seen_quote;
        path++;
    }

    return (WCHAR *)path;
}

UINT WINAPI PathGetCharTypeW(WCHAR ch)
{
    UINT flags = 0;

    TRACE("%#x\n", ch);

    if (!ch || ch < ' ' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '/')
        flags = GCT_INVALID; /* Invalid */
    else if (ch == '*' || ch == '?')
        flags = GCT_WILD; /* Wildchars */
    else if (ch == '\\' || ch == ':')
        return GCT_SEPARATOR; /* Path separators */
    else
    {
        if (ch < 126)
        {
            if (((ch & 0x1) && ch != ';') || !ch || isalnum(ch) || ch == '$' || ch == '&' || ch == '(' ||
                    ch == '.' || ch == '@' || ch == '^' || ch == '\'' || ch == 130 || ch == '`')
            {
                flags |= GCT_SHORTCHAR; /* All these are valid for DOS */
            }
        }
        else
            flags |= GCT_SHORTCHAR; /* Bug compatible with win32 */

        flags |= GCT_LFNCHAR; /* Valid for long file names */
    }

    return flags;
}

UINT WINAPI PathGetCharTypeA(UCHAR ch)
{
    return PathGetCharTypeW(ch);
}

int WINAPI PathGetDriveNumberA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (path && !IsDBCSLeadByte(*path) && path[1] == ':' && tolower(*path) >= 'a' && tolower(*path) <= 'z')
        return tolower(*path) - 'a';

    return -1;
}

int WINAPI PathGetDriveNumberW(const WCHAR *path)
{
    static const WCHAR nt_prefixW[] = {'\\','\\','?','\\'};
    WCHAR drive;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return -1;

    if (!strncmpW(path, nt_prefixW, 4))
        path += 4;

    drive = tolowerW(path[0]);
    if (drive < 'a' || drive > 'z' || path[1] != ':')
        return -1;

    return drive - 'a';
}

BOOL WINAPI PathIsFileSpecA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return FALSE;

    while (*path)
    {
        if (*path == '\\' || *path == ':')
            return FALSE;
        path = CharNextA(path);
    }

    return TRUE;
}

BOOL WINAPI PathIsFileSpecW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return FALSE;

    while (*path)
    {
        if (*path == '\\' || *path == ':')
            return FALSE;
        path++;
    }

    return TRUE;
}

BOOL WINAPI PathIsUNCServerA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!(path && path[0] == '\\' && path[1] == '\\'))
        return FALSE;

    while (*path)
    {
        if (*path == '\\')
            return FALSE;
        path = CharNextA(path);
    }

    return TRUE;
}

BOOL WINAPI PathIsUNCServerW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!(path && path[0] == '\\' && path[1] == '\\'))
        return FALSE;

    return !strchrW(path + 2, '\\');
}

void WINAPI PathRemoveBlanksA(char *path)
{
    char *start;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path)
        return;

    start = path;

    while (*path == ' ')
        path = CharNextA(path);

    while (*path)
        *start++ = *path++;

    if (start != path)
        while (start[-1] == ' ')
            start--;

    *start = '\0';
}

void WINAPI PathRemoveBlanksW(WCHAR *path)
{
    WCHAR *start = path;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return;

    while (*path == ' ')
        path++;

    while (*path)
        *start++ = *path++;

    if (start != path)
        while (start[-1] == ' ')
            start--;

    *start = '\0';
}

void WINAPI PathRemoveExtensionA(char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return;

    path = PathFindExtensionA(path);
    if (path && !*path)
        *path = '\0';
}

void WINAPI PathRemoveExtensionW(WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return;

    path = PathFindExtensionW(path);
    if (path && !*path)
        *path = '\0';
}

BOOL WINAPI PathRenameExtensionA(char *path, const char *ext)
{
    char *extension;

    TRACE("%s, %s\n", wine_dbgstr_a(path), wine_dbgstr_a(ext));

    extension = PathFindExtensionA(path);

    if (!extension || (extension - path + strlen(ext) >= MAX_PATH))
        return FALSE;

    strcpy(extension, ext);
    return TRUE;
}

BOOL WINAPI PathRenameExtensionW(WCHAR *path, const WCHAR *ext)
{
    WCHAR *extension;

    TRACE("%s, %s\n", wine_dbgstr_w(path), wine_dbgstr_w(ext));

    extension = PathFindExtensionW(path);

    if (!extension || (extension - path + strlenW(ext) >= MAX_PATH))
        return FALSE;

    strcpyW(extension, ext);
    return TRUE;
}

void WINAPI PathUnquoteSpacesA(char *path)
{
    unsigned int len;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || *path != '"')
        return;

    len = strlen(path) - 1;
    if (path[len] == '"')
    {
        path[len] = '\0';
        for (; *path; path++)
            *path = path[1];
    }
}

void WINAPI PathUnquoteSpacesW(WCHAR *path)
{
    unsigned int len;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || *path != '"')
        return;

    len = strlenW(path) - 1;
    if (path[len] == '"')
    {
        path[len] = '\0';
        for (; *path; path++)
            *path = path[1];
    }
}

char * WINAPI PathRemoveBackslashA(char *path)
{
    char *ptr;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return NULL;

    ptr = CharPrevA(path, path + strlen(path));
    if (!PathIsRootA(path) && *ptr == '\\')
        *ptr = '\0';

    return ptr;
}

WCHAR * WINAPI PathRemoveBackslashW(WCHAR *path)
{
    WCHAR *ptr;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return NULL;

    ptr = path + strlenW(path);
    if (ptr > path) ptr--;
    if (!PathIsRootW(path) && *ptr == '\\')
      *ptr = '\0';

    return ptr;
}

BOOL WINAPI PathIsLFNFileSpecA(const char *path)
{
    unsigned int name_len = 0, ext_len = 0;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return FALSE;

    while (*path)
    {
        if (*path == ' ')
            return TRUE; /* DOS names cannot have spaces */
        if (*path == '.')
        {
            if (ext_len)
                return TRUE; /* DOS names have only one dot */
            ext_len = 1;
        }
        else if (ext_len)
        {
            ext_len++;
            if (ext_len > 4)
                return TRUE; /* DOS extensions are <= 3 chars*/
        }
        else
        {
            name_len++;
            if (name_len > 8)
                return TRUE; /* DOS names are <= 8 chars */
        }
        path = CharNextA(path);
    }

    return FALSE; /* Valid DOS path */
}

BOOL WINAPI PathIsLFNFileSpecW(const WCHAR *path)
{
    unsigned int name_len = 0, ext_len = 0;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return FALSE;

    while (*path)
    {
        if (*path == ' ')
            return TRUE; /* DOS names cannot have spaces */
        if (*path == '.')
        {
            if (ext_len)
                return TRUE; /* DOS names have only one dot */
            ext_len = 1;
        }
        else if (ext_len)
        {
            ext_len++;
            if (ext_len > 4)
                return TRUE; /* DOS extensions are <= 3 chars*/
        }
        else
        {
            name_len++;
            if (name_len > 8)
                return TRUE; /* DOS names are <= 8 chars */
        }
        path++;
    }

    return FALSE; /* Valid DOS path */
}

#define PATH_CHAR_CLASS_LETTER      0x00000001
#define PATH_CHAR_CLASS_ASTERIX     0x00000002
#define PATH_CHAR_CLASS_DOT         0x00000004
#define PATH_CHAR_CLASS_BACKSLASH   0x00000008
#define PATH_CHAR_CLASS_COLON       0x00000010
#define PATH_CHAR_CLASS_SEMICOLON   0x00000020
#define PATH_CHAR_CLASS_COMMA       0x00000040
#define PATH_CHAR_CLASS_SPACE       0x00000080
#define PATH_CHAR_CLASS_OTHER_VALID 0x00000100
#define PATH_CHAR_CLASS_DOUBLEQUOTE 0x00000200

#define PATH_CHAR_CLASS_INVALID     0x00000000
#define PATH_CHAR_CLASS_ANY         0xffffffff

static const DWORD path_charclass[] =
{
    /* 0x00 */  PATH_CHAR_CLASS_INVALID,      /* 0x01 */  PATH_CHAR_CLASS_INVALID,
    /* 0x02 */  PATH_CHAR_CLASS_INVALID,      /* 0x03 */  PATH_CHAR_CLASS_INVALID,
    /* 0x04 */  PATH_CHAR_CLASS_INVALID,      /* 0x05 */  PATH_CHAR_CLASS_INVALID,
    /* 0x06 */  PATH_CHAR_CLASS_INVALID,      /* 0x07 */  PATH_CHAR_CLASS_INVALID,
    /* 0x08 */  PATH_CHAR_CLASS_INVALID,      /* 0x09 */  PATH_CHAR_CLASS_INVALID,
    /* 0x0a */  PATH_CHAR_CLASS_INVALID,      /* 0x0b */  PATH_CHAR_CLASS_INVALID,
    /* 0x0c */  PATH_CHAR_CLASS_INVALID,      /* 0x0d */  PATH_CHAR_CLASS_INVALID,
    /* 0x0e */  PATH_CHAR_CLASS_INVALID,      /* 0x0f */  PATH_CHAR_CLASS_INVALID,
    /* 0x10 */  PATH_CHAR_CLASS_INVALID,      /* 0x11 */  PATH_CHAR_CLASS_INVALID,
    /* 0x12 */  PATH_CHAR_CLASS_INVALID,      /* 0x13 */  PATH_CHAR_CLASS_INVALID,
    /* 0x14 */  PATH_CHAR_CLASS_INVALID,      /* 0x15 */  PATH_CHAR_CLASS_INVALID,
    /* 0x16 */  PATH_CHAR_CLASS_INVALID,      /* 0x17 */  PATH_CHAR_CLASS_INVALID,
    /* 0x18 */  PATH_CHAR_CLASS_INVALID,      /* 0x19 */  PATH_CHAR_CLASS_INVALID,
    /* 0x1a */  PATH_CHAR_CLASS_INVALID,      /* 0x1b */  PATH_CHAR_CLASS_INVALID,
    /* 0x1c */  PATH_CHAR_CLASS_INVALID,      /* 0x1d */  PATH_CHAR_CLASS_INVALID,
    /* 0x1e */  PATH_CHAR_CLASS_INVALID,      /* 0x1f */  PATH_CHAR_CLASS_INVALID,
    /* ' '  */  PATH_CHAR_CLASS_SPACE,        /* '!'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '"'  */  PATH_CHAR_CLASS_DOUBLEQUOTE,  /* '#'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '$'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '%'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '&'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '\'' */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '('  */  PATH_CHAR_CLASS_OTHER_VALID,  /* ')'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '*'  */  PATH_CHAR_CLASS_ASTERIX,      /* '+'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* ','  */  PATH_CHAR_CLASS_COMMA,        /* '-'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '.'  */  PATH_CHAR_CLASS_DOT,          /* '/'  */  PATH_CHAR_CLASS_INVALID,
    /* '0'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '1'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '2'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '3'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '4'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '5'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '6'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '7'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '8'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '9'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* ':'  */  PATH_CHAR_CLASS_COLON,        /* ';'  */  PATH_CHAR_CLASS_SEMICOLON,
    /* '<'  */  PATH_CHAR_CLASS_INVALID,      /* '='  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '>'  */  PATH_CHAR_CLASS_INVALID,      /* '?'  */  PATH_CHAR_CLASS_LETTER,
    /* '@'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* 'A'  */  PATH_CHAR_CLASS_ANY,
    /* 'B'  */  PATH_CHAR_CLASS_ANY,          /* 'C'  */  PATH_CHAR_CLASS_ANY,
    /* 'D'  */  PATH_CHAR_CLASS_ANY,          /* 'E'  */  PATH_CHAR_CLASS_ANY,
    /* 'F'  */  PATH_CHAR_CLASS_ANY,          /* 'G'  */  PATH_CHAR_CLASS_ANY,
    /* 'H'  */  PATH_CHAR_CLASS_ANY,          /* 'I'  */  PATH_CHAR_CLASS_ANY,
    /* 'J'  */  PATH_CHAR_CLASS_ANY,          /* 'K'  */  PATH_CHAR_CLASS_ANY,
    /* 'L'  */  PATH_CHAR_CLASS_ANY,          /* 'M'  */  PATH_CHAR_CLASS_ANY,
    /* 'N'  */  PATH_CHAR_CLASS_ANY,          /* 'O'  */  PATH_CHAR_CLASS_ANY,
    /* 'P'  */  PATH_CHAR_CLASS_ANY,          /* 'Q'  */  PATH_CHAR_CLASS_ANY,
    /* 'R'  */  PATH_CHAR_CLASS_ANY,          /* 'S'  */  PATH_CHAR_CLASS_ANY,
    /* 'T'  */  PATH_CHAR_CLASS_ANY,          /* 'U'  */  PATH_CHAR_CLASS_ANY,
    /* 'V'  */  PATH_CHAR_CLASS_ANY,          /* 'W'  */  PATH_CHAR_CLASS_ANY,
    /* 'X'  */  PATH_CHAR_CLASS_ANY,          /* 'Y'  */  PATH_CHAR_CLASS_ANY,
    /* 'Z'  */  PATH_CHAR_CLASS_ANY,          /* '['  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '\\' */  PATH_CHAR_CLASS_BACKSLASH,    /* ']'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '^'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* '_'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '`'  */  PATH_CHAR_CLASS_OTHER_VALID,  /* 'a'  */  PATH_CHAR_CLASS_ANY,
    /* 'b'  */  PATH_CHAR_CLASS_ANY,          /* 'c'  */  PATH_CHAR_CLASS_ANY,
    /* 'd'  */  PATH_CHAR_CLASS_ANY,          /* 'e'  */  PATH_CHAR_CLASS_ANY,
    /* 'f'  */  PATH_CHAR_CLASS_ANY,          /* 'g'  */  PATH_CHAR_CLASS_ANY,
    /* 'h'  */  PATH_CHAR_CLASS_ANY,          /* 'i'  */  PATH_CHAR_CLASS_ANY,
    /* 'j'  */  PATH_CHAR_CLASS_ANY,          /* 'k'  */  PATH_CHAR_CLASS_ANY,
    /* 'l'  */  PATH_CHAR_CLASS_ANY,          /* 'm'  */  PATH_CHAR_CLASS_ANY,
    /* 'n'  */  PATH_CHAR_CLASS_ANY,          /* 'o'  */  PATH_CHAR_CLASS_ANY,
    /* 'p'  */  PATH_CHAR_CLASS_ANY,          /* 'q'  */  PATH_CHAR_CLASS_ANY,
    /* 'r'  */  PATH_CHAR_CLASS_ANY,          /* 's'  */  PATH_CHAR_CLASS_ANY,
    /* 't'  */  PATH_CHAR_CLASS_ANY,          /* 'u'  */  PATH_CHAR_CLASS_ANY,
    /* 'v'  */  PATH_CHAR_CLASS_ANY,          /* 'w'  */  PATH_CHAR_CLASS_ANY,
    /* 'x'  */  PATH_CHAR_CLASS_ANY,          /* 'y'  */  PATH_CHAR_CLASS_ANY,
    /* 'z'  */  PATH_CHAR_CLASS_ANY,          /* '{'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '|'  */  PATH_CHAR_CLASS_INVALID,      /* '}'  */  PATH_CHAR_CLASS_OTHER_VALID,
    /* '~'  */  PATH_CHAR_CLASS_OTHER_VALID
};

BOOL WINAPI PathIsValidCharA(char c, DWORD class)
{
    if ((unsigned)c > 0x7e)
        return class & PATH_CHAR_CLASS_OTHER_VALID;

    return class & path_charclass[(unsigned)c];
}

BOOL WINAPI PathIsValidCharW(WCHAR c, DWORD class)
{
    if (c > 0x7e)
        return class & PATH_CHAR_CLASS_OTHER_VALID;

    return class & path_charclass[c];
}

char * WINAPI PathFindNextComponentA(const char *path)
{
    char *slash;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path)
        return NULL;

    if ((slash = StrChrA(path, '\\')))
    {
        if (slash[1] == '\\')
            slash++;
        return slash + 1;
    }

    return (char *)path + strlen(path);
}

WCHAR * WINAPI PathFindNextComponentW(const WCHAR *path)
{
    WCHAR *slash;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return NULL;

    if ((slash = StrChrW(path, '\\')))
    {
        if (slash[1] == '\\')
            slash++;
        return slash + 1;
    }

    return (WCHAR *)path + strlenW(path);
}

char * WINAPI PathSkipRootA(const char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path)
        return NULL;

    if (*path == '\\' && path[1] == '\\')
    {
        /* Network share: skip share server and mount point */
        path += 2;
        if ((path = StrChrA(path, '\\')) && (path = StrChrA(path + 1, '\\')))
            path++;
        return (char *)path;
    }

    if (IsDBCSLeadByte(*path))
        return NULL;

    /* Check x:\ */
    if (path[0] && path[1] == ':' && path[2] == '\\')
        return (char *)path + 3;

    return NULL;
}

WCHAR * WINAPI PathSkipRootW(const WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return NULL;

    if (*path == '\\' && path[1] == '\\')
    {
        /* Network share: skip share server and mount point */
        path += 2;
        if ((path = StrChrW(path, '\\')) && (path = StrChrW(path + 1, '\\')))
            path++;
        return (WCHAR *)path;
    }

    /* Check x:\ */
    if (path[0] && path[1] == ':' && path[2] == '\\')
        return (WCHAR *)path + 3;

    return NULL;
}

void WINAPI PathStripPathA(char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (path)
    {
        char *filename = PathFindFileNameA(path);
        if (filename != path)
            RtlMoveMemory(path, filename, strlen(filename) + 1);
    }
}

void WINAPI PathStripPathW(WCHAR *path)
{
    WCHAR *filename;

    TRACE("%s\n", wine_dbgstr_w(path));
    filename = PathFindFileNameW(path);
    if (filename != path)
        RtlMoveMemory(path, filename, (strlenW(filename) + 1) * sizeof(WCHAR));
}

BOOL WINAPI PathSearchAndQualifyA(const char *path, char *buffer, UINT length)
{
    TRACE("%s, %p, %u\n", wine_dbgstr_a(path), buffer, length);

    if (SearchPathA(NULL, path, NULL, length, buffer, NULL))
        return TRUE;

    return !!GetFullPathNameA(path, length, buffer, NULL);
}

BOOL WINAPI PathSearchAndQualifyW(const WCHAR *path, WCHAR *buffer, UINT length)
{
    TRACE("%s, %p, %u\n", wine_dbgstr_w(path), buffer, length);

    if (SearchPathW(NULL, path, NULL, length, buffer, NULL))
        return TRUE;
    return !!GetFullPathNameW(path, length, buffer, NULL);
}

BOOL WINAPI PathRelativePathToA(char *path, const char *from, DWORD attributes_from, const char *to,
        DWORD attributes_to)
{
    WCHAR pathW[MAX_PATH], fromW[MAX_PATH], toW[MAX_PATH];
    BOOL ret;

    TRACE("%p, %s, %#x, %s, %#x\n", path, wine_dbgstr_a(from), attributes_from, wine_dbgstr_a(to), attributes_to);

    if (!path || !from || !to)
        return FALSE;

    MultiByteToWideChar(CP_ACP, 0, from, -1, fromW, ARRAY_SIZE(fromW));
    MultiByteToWideChar(CP_ACP, 0, to, -1, toW, ARRAY_SIZE(toW));
    ret = PathRelativePathToW(pathW, fromW, attributes_from, toW, attributes_to);
    WideCharToMultiByte(CP_ACP, 0, pathW, -1, path, MAX_PATH, 0, 0);

    return ret;
}

BOOL WINAPI PathRelativePathToW(WCHAR *path, const WCHAR *from, DWORD attributes_from, const WCHAR *to,
        DWORD attributes_to)
{
    static const WCHAR szPrevDirSlash[] = { '.', '.', '\\', '\0' };
    static const WCHAR szPrevDir[] = { '.', '.', '\0' };
    WCHAR fromW[MAX_PATH], toW[MAX_PATH];
    DWORD len;

    TRACE("%p, %s, %#x, %s, %#x\n", path, wine_dbgstr_w(from), attributes_from, wine_dbgstr_w(to), attributes_to);

    if (!path || !from || !to)
        return FALSE;

    *path = '\0';
    lstrcpynW(fromW, from, ARRAY_SIZE(fromW));
    lstrcpynW(toW, to, ARRAY_SIZE(toW));

    if (!(attributes_from & FILE_ATTRIBUTE_DIRECTORY))
        PathRemoveFileSpecW(fromW);
    if (!(attributes_to & FILE_ATTRIBUTE_DIRECTORY))
        PathRemoveFileSpecW(toW);

    /* Paths can only be relative if they have a common root */
    if (!(len = PathCommonPrefixW(fromW, toW, 0)))
        return FALSE;

    /* Strip off 'from' components to the root, by adding "..\" */
    from = fromW + len;
    if (!*from)
    {
        path[0] = '.';
        path[1] = '\0';
    }
    if (*from == '\\')
        from++;

    while (*from)
    {
        from = PathFindNextComponentW(from);
        strcatW(path, *from ? szPrevDirSlash : szPrevDir);
    }

    /* From the root add the components of 'to' */
    to += len;
    /* We check to[-1] to avoid skipping end of string. See the notes for this function. */
    if (*to && to[-1])
    {
        if (*to != '\\')
            to--;
        len = strlenW(path);
        if (len + strlenW(to) >= MAX_PATH)
        {
            *path = '\0';
            return FALSE;
        }
        strcpyW(path + len, to);
    }

    return TRUE;
}

static BOOL path_match_maskA(const char *name, const char *mask)
{
    while (*name && *mask && *mask != ';')
    {
        if (*mask == '*')
        {
            do
            {
                if (path_match_maskA(name, mask + 1))
                    return TRUE;  /* try substrings */
            } while (*name++);
            return FALSE;
        }

        if (toupper(*mask) != toupper(*name) && *mask != '?')
            return FALSE;

        name = CharNextA(name);
        mask = CharNextA(mask);
    }

    if (!*name)
    {
        while (*mask == '*')
            mask++;
        if (!*mask || *mask == ';')
            return TRUE;
    }

    return FALSE;
}


BOOL WINAPI PathMatchSpecA(const char *path, const char *mask)
{
    TRACE("%s, %s\n", wine_dbgstr_a(path), wine_dbgstr_a(mask));

    if (!lstrcmpA(mask, "*.*"))
        return TRUE; /* Matches every path */

    while (*mask)
    {
        while (*mask == ' ')
            mask++; /* Eat leading spaces */

        if (path_match_maskA(path, mask))
            return TRUE; /* Matches the current mask */

        while (*mask && *mask != ';')
            mask = CharNextA(mask); /* masks separated by ';' */

        if (*mask == ';')
            mask++;
    }

    return FALSE;
}

static BOOL path_match_maskW(const WCHAR *name, const WCHAR *mask)
{
    while (*name && *mask && *mask != ';')
    {
        if (*mask == '*')
        {
            do
            {
                if (path_match_maskW(name, mask + 1))
                    return TRUE;  /* try substrings */
            } while (*name++);
            return FALSE;
        }

        if (toupperW(*mask) != toupperW(*name) && *mask != '?')
            return FALSE;

        name++;
        mask++;
    }

    if (!*name)
    {
        while (*mask == '*')
            mask++;
        if (!*mask || *mask == ';')
            return TRUE;
    }

    return FALSE;
}

BOOL WINAPI PathMatchSpecW(const WCHAR *path, const WCHAR *mask)
{
    static const WCHAR maskallW[] = {'*','.','*',0};

    TRACE("%s, %s\n", wine_dbgstr_w(path), wine_dbgstr_w(mask));

    if (!lstrcmpW(mask, maskallW))
        return TRUE; /* Matches every path */

    while (*mask)
    {
        while (*mask == ' ')
            mask++; /* Eat leading spaces */

        if (path_match_maskW(path, mask))
            return TRUE; /* Matches the current path */

        while (*mask && *mask != ';')
            mask++; /* masks separated by ';' */

        if (*mask == ';')
            mask++;
    }

    return FALSE;
}

void WINAPI PathQuoteSpacesA(char *path)
{
    TRACE("%s\n", wine_dbgstr_a(path));

    if (path && StrChrA(path, ' '))
    {
        size_t len = strlen(path) + 1;

        if (len + 2 < MAX_PATH)
        {
            memmove(path + 1, path, len);
            path[0] = '"';
            path[len] = '"';
            path[len + 1] = '\0';
        }
    }
}

void WINAPI PathQuoteSpacesW(WCHAR *path)
{
    TRACE("%s\n", wine_dbgstr_w(path));

    if (path && StrChrW(path, ' '))
    {
        int len = strlenW(path) + 1;

        if (len + 2 < MAX_PATH)
        {
            memmove(path + 1, path, len * sizeof(WCHAR));
            path[0] = '"';
            path[len] = '"';
            path[len + 1] = '\0';
        }
    }
}

BOOL WINAPI PathIsSameRootA(const char *path1, const char *path2)
{
    const char *start;
    int len;

    TRACE("%s, %s\n", wine_dbgstr_a(path1), wine_dbgstr_a(path2));

    if (!path1 || !path2 || !(start = PathSkipRootA(path1)))
        return FALSE;

    len = PathCommonPrefixA(path1, path2, NULL) + 1;
    return start - path1 <= len;
}

BOOL WINAPI PathIsSameRootW(const WCHAR *path1, const WCHAR *path2)
{
    const WCHAR *start;
    int len;

    TRACE("%s, %s\n", wine_dbgstr_w(path1), wine_dbgstr_w(path2));

    if (!path1 || !path2 || !(start = PathSkipRootW(path1)))
        return FALSE;

    len = PathCommonPrefixW(path1, path2, NULL) + 1;
    return start - path1 <= len;
}

BOOL WINAPI PathFileExistsA(const char *path)
{
    UINT prev_mode;
    DWORD attrs;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path)
        return FALSE;

    /* Prevent a dialog box if path is on a disk that has been ejected. */
    prev_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    attrs = GetFileAttributesA(path);
    SetErrorMode(prev_mode);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

BOOL WINAPI PathFileExistsW(const WCHAR *path)
{
    UINT prev_mode;
    DWORD attrs;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path)
        return FALSE;

    prev_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    attrs = GetFileAttributesW(path);
    SetErrorMode(prev_mode);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

static const struct
{
    URL_SCHEME scheme_number;
    WCHAR scheme_name[12];
}
url_schemes[] =
{
    { URL_SCHEME_FTP,        {'f','t','p',0}},
    { URL_SCHEME_HTTP,       {'h','t','t','p',0}},
    { URL_SCHEME_GOPHER,     {'g','o','p','h','e','r',0}},
    { URL_SCHEME_MAILTO,     {'m','a','i','l','t','o',0}},
    { URL_SCHEME_NEWS,       {'n','e','w','s',0}},
    { URL_SCHEME_NNTP,       {'n','n','t','p',0}},
    { URL_SCHEME_TELNET,     {'t','e','l','n','e','t',0}},
    { URL_SCHEME_WAIS,       {'w','a','i','s',0}},
    { URL_SCHEME_FILE,       {'f','i','l','e',0}},
    { URL_SCHEME_MK,         {'m','k',0}},
    { URL_SCHEME_HTTPS,      {'h','t','t','p','s',0}},
    { URL_SCHEME_SHELL,      {'s','h','e','l','l',0}},
    { URL_SCHEME_SNEWS,      {'s','n','e','w','s',0}},
    { URL_SCHEME_LOCAL,      {'l','o','c','a','l',0}},
    { URL_SCHEME_JAVASCRIPT, {'j','a','v','a','s','c','r','i','p','t',0}},
    { URL_SCHEME_VBSCRIPT,   {'v','b','s','c','r','i','p','t',0}},
    { URL_SCHEME_ABOUT,      {'a','b','o','u','t',0}},
    { URL_SCHEME_RES,        {'r','e','s',0}},
};

static DWORD get_scheme_code(const WCHAR *scheme, DWORD scheme_len)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(url_schemes); ++i)
    {
        if (scheme_len == strlenW(url_schemes[i].scheme_name)
                && !strncmpiW(scheme, url_schemes[i].scheme_name, scheme_len))
            return url_schemes[i].scheme_number;
    }

    return URL_SCHEME_UNKNOWN;
}

HRESULT WINAPI ParseURLA(const char *url, PARSEDURLA *result)
{
    WCHAR scheme[INTERNET_MAX_SCHEME_LENGTH];
    const char *ptr = url;
    int len;

    TRACE("%s, %p\n", wine_dbgstr_a(url), result);

    if (result->cbSize != sizeof(*result))
        return E_INVALIDARG;

    while (*ptr && (isalnum(*ptr) || *ptr == '-' || *ptr == '+' || *ptr == '.'))
        ptr++;

    if (*ptr != ':' || ptr <= url + 1)
    {
        result->pszProtocol = NULL;
        return URL_E_INVALID_SYNTAX;
    }

    result->pszProtocol = url;
    result->cchProtocol = ptr - url;
    result->pszSuffix = ptr + 1;
    result->cchSuffix = strlen(result->pszSuffix);

    len = MultiByteToWideChar(CP_ACP, 0, url, ptr - url, scheme, ARRAY_SIZE(scheme));
    result->nScheme = get_scheme_code(scheme, len);

    return S_OK;
}

HRESULT WINAPI ParseURLW(const WCHAR *url, PARSEDURLW *result)
{
    const WCHAR *ptr = url;

    TRACE("%s, %p\n", wine_dbgstr_w(url), result);

    if (result->cbSize != sizeof(*result))
        return E_INVALIDARG;

    while (*ptr && (isalnumW(*ptr) || *ptr == '-' || *ptr == '+' || *ptr == '.'))
        ptr++;

    if (*ptr != ':' || ptr <= url + 1)
    {
        result->pszProtocol = NULL;
        return URL_E_INVALID_SYNTAX;
    }

    result->pszProtocol = url;
    result->cchProtocol = ptr - url;
    result->pszSuffix = ptr + 1;
    result->cchSuffix = strlenW(result->pszSuffix);
    result->nScheme = get_scheme_code(url, ptr - url);

    return S_OK;
}

HRESULT WINAPI UrlUnescapeA(char *url, char *unescaped, DWORD *unescaped_len, DWORD flags)
{
    BOOL stop_unescaping = FALSE;
    const char *src;
    char *dst, next;
    DWORD needed;
    HRESULT hr;

    TRACE("%s, %p, %p, %#x\n", wine_dbgstr_a(url), unescaped, unescaped_len, flags);

    if (!url)
        return E_INVALIDARG;

    if (flags & URL_UNESCAPE_INPLACE)
        dst = url;
    else
    {
        if (!unescaped || !unescaped_len) return E_INVALIDARG;
        dst = unescaped;
    }

    for (src = url, needed = 0; *src; src++, needed++)
    {
        if (flags & URL_DONT_UNESCAPE_EXTRA_INFO && (*src == '#' || *src == '?'))
        {
            stop_unescaping = TRUE;
            next = *src;
        }
        else if (*src == '%' && isxdigit(*(src + 1)) && isxdigit(*(src + 2)) && !stop_unescaping)
        {
            INT ih;
            char buf[3];
            memcpy(buf, src + 1, 2);
            buf[2] = '\0';
            ih = strtol(buf, NULL, 16);
            next = (CHAR) ih;
            src += 2; /* Advance to end of escape */
        }
        else
            next = *src;

        if (flags & URL_UNESCAPE_INPLACE || needed < *unescaped_len)
            *dst++ = next;
    }

    if (flags & URL_UNESCAPE_INPLACE || needed < *unescaped_len)
    {
        *dst = '\0';
        hr = S_OK;
    }
    else
    {
        needed++; /* add one for the '\0' */
        hr = E_POINTER;
    }

    if (!(flags & URL_UNESCAPE_INPLACE))
        *unescaped_len = needed;

    if (hr == S_OK)
        TRACE("result %s\n", flags & URL_UNESCAPE_INPLACE ? wine_dbgstr_a(url) : wine_dbgstr_a(unescaped));

    return hr;
}

HRESULT WINAPI UrlUnescapeW(WCHAR *url, WCHAR *unescaped, DWORD *unescaped_len, DWORD flags)
{
    BOOL stop_unescaping = FALSE;
    const WCHAR *src;
    WCHAR *dst, next;
    DWORD needed;
    HRESULT hr;

    TRACE("%s, %p, %p, %#x\n", wine_dbgstr_w(url), unescaped, unescaped_len, flags);

    if (!url)
        return E_INVALIDARG;

    if (flags & URL_UNESCAPE_INPLACE)
        dst = url;
    else
    {
        if (!unescaped || !unescaped_len) return E_INVALIDARG;
        dst = unescaped;
    }

    for (src = url, needed = 0; *src; src++, needed++)
    {
        if (flags & URL_DONT_UNESCAPE_EXTRA_INFO && (*src == '#' || *src == '?'))
        {
            stop_unescaping = TRUE;
            next = *src;
        }
        else if (*src == '%' && isxdigitW(*(src + 1)) && isxdigitW(*(src + 2)) && !stop_unescaping)
        {
            INT ih;
            WCHAR buf[5] = {'0','x',0};
            memcpy(buf + 2, src + 1, 2*sizeof(WCHAR));
            buf[4] = 0;
            StrToIntExW(buf, STIF_SUPPORT_HEX, &ih);
            next = (WCHAR) ih;
            src += 2; /* Advance to end of escape */
        }
        else
            next = *src;

        if (flags & URL_UNESCAPE_INPLACE || needed < *unescaped_len)
            *dst++ = next;
    }

    if (flags & URL_UNESCAPE_INPLACE || needed < *unescaped_len)
    {
        *dst = '\0';
        hr = S_OK;
    }
    else
    {
        needed++; /* add one for the '\0' */
        hr = E_POINTER;
    }

    if (!(flags & URL_UNESCAPE_INPLACE))
        *unescaped_len = needed;

    if (hr == S_OK)
        TRACE("result %s\n", flags & URL_UNESCAPE_INPLACE ? wine_dbgstr_w(url) : wine_dbgstr_w(unescaped));

    return hr;
}

HRESULT WINAPI PathCreateFromUrlA(const char *pszUrl, char *pszPath, DWORD *pcchPath, DWORD dwReserved)
{
    WCHAR bufW[MAX_PATH];
    WCHAR *pathW = bufW;
    UNICODE_STRING urlW;
    HRESULT ret;
    DWORD lenW = ARRAY_SIZE(bufW), lenA;

    if (!pszUrl || !pszPath || !pcchPath || !*pcchPath)
        return E_INVALIDARG;

    if(!RtlCreateUnicodeStringFromAsciiz(&urlW, pszUrl))
        return E_INVALIDARG;
    if((ret = PathCreateFromUrlW(urlW.Buffer, pathW, &lenW, dwReserved)) == E_POINTER) {
        pathW = HeapAlloc(GetProcessHeap(), 0, lenW * sizeof(WCHAR));
        ret = PathCreateFromUrlW(urlW.Buffer, pathW, &lenW, dwReserved);
    }
    if(ret == S_OK) {
        RtlUnicodeToMultiByteSize(&lenA, pathW, lenW * sizeof(WCHAR));
        if(*pcchPath > lenA) {
            RtlUnicodeToMultiByteN(pszPath, *pcchPath - 1, &lenA, pathW, lenW * sizeof(WCHAR));
            pszPath[lenA] = 0;
            *pcchPath = lenA;
        } else {
            *pcchPath = lenA + 1;
            ret = E_POINTER;
        }
    }
    if(pathW != bufW) HeapFree(GetProcessHeap(), 0, pathW);
    RtlFreeUnicodeString(&urlW);
    return ret;
}

HRESULT WINAPI PathCreateFromUrlW(const WCHAR *url, WCHAR *path, DWORD *pcchPath, DWORD dwReserved)
{
    static const WCHAR file_colon[] = { 'f','i','l','e',':',0 };
    static const WCHAR localhost[] = { 'l','o','c','a','l','h','o','s','t',0 };
    DWORD nslashes, unescape, len;
    const WCHAR *src;
    WCHAR *tpath, *dst;
    HRESULT hr = S_OK;

    TRACE("%s, %p, %p, %#x\n", wine_dbgstr_w(url), path, pcchPath, dwReserved);

    if (!url || !path || !pcchPath || !*pcchPath)
        return E_INVALIDARG;

    if (lstrlenW(url) < 5)
        return E_INVALIDARG;

    if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, url, 5, file_colon, 5) != CSTR_EQUAL)
        return E_INVALIDARG;

    url += 5;

    src = url;
    nslashes = 0;
    while (*src == '/' || *src == '\\')
    {
        nslashes++;
        src++;
    }

    /* We need a temporary buffer so we can compute what size to ask for.
     * We know that the final string won't be longer than the current pszUrl
     * plus at most two backslashes. All the other transformations make it
     * shorter.
     */
    len = 2 + lstrlenW(url) + 1;
    if (*pcchPath < len)
        tpath = heap_alloc(len * sizeof(WCHAR));
    else
        tpath = path;

    len = 0;
    dst = tpath;
    unescape = 1;
    switch (nslashes)
    {
    case 0:
        /* 'file:' + escaped DOS path */
        break;
    case 1:
        /* 'file:/' + escaped DOS path */
        /* fall through */
    case 3:
        /* 'file:///' (implied localhost) + escaped DOS path */
        if (!isalphaW(*src) || (src[1] != ':' && src[1] != '|'))
            src -= 1;
        break;
    case 2:
        if (lstrlenW(src) >= 10 && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE,
            src, 9, localhost, 9) == CSTR_EQUAL && (src[9] == '/' || src[9] == '\\'))
        {
            /* 'file://localhost/' + escaped DOS path */
            src += 10;
        }
        else if (isalphaW(*src) && (src[1] == ':' || src[1] == '|'))
        {
            /* 'file://' + unescaped DOS path */
            unescape = 0;
        }
        else
        {
            /*    'file://hostname:port/path' (where path is escaped)
             * or 'file:' + escaped UNC path (\\server\share\path)
             * The second form is clearly specific to Windows and it might
             * even be doing a network lookup to try to figure it out.
             */
            while (*src && *src != '/' && *src != '\\')
                src++;
            len = src - url;
            StrCpyNW(dst, url, len + 1);
            dst += len;
            if (*src && isalphaW(src[1]) && (src[2] == ':' || src[2] == '|'))
            {
                /* 'Forget' to add a trailing '/', just like Windows */
                src++;
            }
        }
        break;
    case 4:
        /* 'file://' + unescaped UNC path (\\server\share\path) */
        unescape = 0;
        if (isalphaW(*src) && (src[1] == ':' || src[1] == '|'))
            break;
        /* fall through */
    default:
        /* 'file:/...' + escaped UNC path (\\server\share\path) */
        src -= 2;
    }

    /* Copy the remainder of the path */
    len += lstrlenW(src);
    strcpyW(dst, src);

     /* First do the Windows-specific path conversions */
    for (dst = tpath; *dst; dst++)
        if (*dst == '/') *dst = '\\';
    if (isalphaW(*tpath) && tpath[1] == '|')
        tpath[1] = ':'; /* c| -> c: */

    /* And only then unescape the path (i.e. escaped slashes are left as is) */
    if (unescape)
    {
        hr = UrlUnescapeW(tpath, NULL, &len, URL_UNESCAPE_INPLACE);
        if (hr == S_OK)
        {
            /* When working in-place UrlUnescapeW() does not set len */
            len = lstrlenW(tpath);
        }
    }

    if (*pcchPath < len + 1)
    {
        hr = E_POINTER;
        *pcchPath = len + 1;
    }
    else
    {
        *pcchPath = len;
        if (tpath != path)
            strcpyW(path, tpath);
    }
    if (tpath != path)
        heap_free(tpath);

    TRACE("Returning (%u) %s\n", *pcchPath, wine_dbgstr_w(path));
    return hr;
}

HRESULT WINAPI PathCreateFromUrlAlloc(const WCHAR *url, WCHAR **path, DWORD reserved)
{
    WCHAR pathW[MAX_PATH];
    DWORD size;
    HRESULT hr;

    size = MAX_PATH;
    hr = PathCreateFromUrlW(url, pathW, &size, reserved);
    if (SUCCEEDED(hr))
    {
        /* Yes, this is supposed to crash if 'path' is NULL */
        *path = StrDupW(pathW);
    }

    return hr;
}

BOOL WINAPI PathIsURLA(const char *path)
{
    PARSEDURLA base;
    HRESULT hr;

    TRACE("%s\n", wine_dbgstr_a(path));

    if (!path || !*path)
        return FALSE;

    /* get protocol */
    base.cbSize = sizeof(base);
    hr = ParseURLA(path, &base);
    return hr == S_OK && (base.nScheme != URL_SCHEME_INVALID);
}

BOOL WINAPI PathIsURLW(const WCHAR *path)
{
    PARSEDURLW base;
    HRESULT hr;

    TRACE("%s\n", wine_dbgstr_w(path));

    if (!path || !*path)
        return FALSE;

    /* get protocol */
    base.cbSize = sizeof(base);
    hr = ParseURLW(path, &base);
    return hr == S_OK && (base.nScheme != URL_SCHEME_INVALID);
}

#define WINE_URL_BASH_AS_SLASH    0x01
#define WINE_URL_COLLAPSE_SLASHES 0x02
#define WINE_URL_ESCAPE_SLASH     0x04
#define WINE_URL_ESCAPE_HASH      0x08
#define WINE_URL_ESCAPE_QUESTION  0x10
#define WINE_URL_STOP_ON_HASH     0x20
#define WINE_URL_STOP_ON_QUESTION 0x40

static BOOL url_needs_escape(WCHAR ch, DWORD flags, DWORD int_flags)
{
    if (flags & URL_ESCAPE_SPACES_ONLY)
        return ch == ' ';

    if ((flags & URL_ESCAPE_PERCENT) && (ch == '%'))
        return TRUE;

    if ((flags & URL_ESCAPE_AS_UTF8) && (ch >= 0x80))
        return TRUE;

    if (ch <= 31 || (ch >= 127 && ch <= 255) )
        return TRUE;

    if (isalnumW(ch))
        return FALSE;

    switch (ch) {
    case ' ':
    case '<':
    case '>':
    case '\"':
    case '{':
    case '}':
    case '|':
    case '\\':
    case '^':
    case ']':
    case '[':
    case '`':
    case '&':
        return TRUE;
    case '/':
        return !!(int_flags & WINE_URL_ESCAPE_SLASH);
    case '?':
        return !!(int_flags & WINE_URL_ESCAPE_QUESTION);
    case '#':
        return !!(int_flags & WINE_URL_ESCAPE_HASH);
    default:
        return FALSE;
    }
}

HRESULT WINAPI UrlEscapeA(const char *url, char *escaped, DWORD *escaped_len, DWORD flags)
{
    WCHAR bufW[INTERNET_MAX_URL_LENGTH];
    WCHAR *escapedW = bufW;
    UNICODE_STRING urlW;
    HRESULT hr;
    DWORD lenW = ARRAY_SIZE(bufW), lenA;

    if (!escaped || !escaped_len || !*escaped_len)
        return E_INVALIDARG;

    if (!RtlCreateUnicodeStringFromAsciiz(&urlW, url))
        return E_INVALIDARG;

    if (flags & URL_ESCAPE_AS_UTF8)
    {
        RtlFreeUnicodeString(&urlW);
        return E_NOTIMPL;
    }

    if ((hr = UrlEscapeW(urlW.Buffer, escapedW, &lenW, flags)) == E_POINTER)
    {
        escapedW = heap_alloc(lenW * sizeof(WCHAR));
        hr = UrlEscapeW(urlW.Buffer, escapedW, &lenW, flags);
    }

    if (hr == S_OK)
    {
        RtlUnicodeToMultiByteSize(&lenA, escapedW, lenW * sizeof(WCHAR));
        if (*escaped_len > lenA)
        {
            RtlUnicodeToMultiByteN(escaped, *escaped_len - 1, &lenA, escapedW, lenW * sizeof(WCHAR));
            escaped[lenA] = 0;
            *escaped_len = lenA;
        }
        else
        {
            *escaped_len = lenA + 1;
            hr = E_POINTER;
        }
    }
    if (escapedW != bufW)
        heap_free(escapedW);
    RtlFreeUnicodeString(&urlW);
    return hr;
}

HRESULT WINAPI UrlEscapeW(const WCHAR *url, WCHAR *escaped, DWORD *escaped_len, DWORD flags)
{
    static const WCHAR localhost[] = {'l','o','c','a','l','h','o','s','t',0};
    DWORD needed = 0, slashes = 0, int_flags;
    WCHAR next[12], *dst, *dst_ptr;
    BOOL stop_escaping = FALSE;
    PARSEDURLW parsed_url;
    const WCHAR *src;
    INT i, len;
    HRESULT hr;

    TRACE("%p, %s, %p, %p, %#x\n", url, wine_dbgstr_w(url), escaped, escaped_len, flags);

    if (!url || !escaped_len || !escaped || *escaped_len == 0)
        return E_INVALIDARG;

    if (flags & ~(URL_ESCAPE_SPACES_ONLY | URL_ESCAPE_SEGMENT_ONLY | URL_DONT_ESCAPE_EXTRA_INFO |
            URL_ESCAPE_PERCENT | URL_ESCAPE_AS_UTF8))
    {
        FIXME("Unimplemented flags: %08x\n", flags);
    }

    dst_ptr = dst = heap_alloc(*escaped_len * sizeof(WCHAR));
    if (!dst_ptr)
        return E_OUTOFMEMORY;

    /* fix up flags */
    if (flags & URL_ESCAPE_SPACES_ONLY)
        /* if SPACES_ONLY specified, reset the other controls */
        flags &= ~(URL_DONT_ESCAPE_EXTRA_INFO | URL_ESCAPE_PERCENT | URL_ESCAPE_SEGMENT_ONLY);
    else
        /* if SPACES_ONLY *not* specified the assume DONT_ESCAPE_EXTRA_INFO */
        flags |= URL_DONT_ESCAPE_EXTRA_INFO;

    int_flags = 0;
    if (flags & URL_ESCAPE_SEGMENT_ONLY)
        int_flags = WINE_URL_ESCAPE_QUESTION | WINE_URL_ESCAPE_HASH | WINE_URL_ESCAPE_SLASH;
    else
    {
        parsed_url.cbSize = sizeof(parsed_url);
        if (ParseURLW(url, &parsed_url) != S_OK)
            parsed_url.nScheme = URL_SCHEME_INVALID;

        TRACE("scheme = %d (%s)\n", parsed_url.nScheme, debugstr_wn(parsed_url.pszProtocol, parsed_url.cchProtocol));

        if (flags & URL_DONT_ESCAPE_EXTRA_INFO)
            int_flags = WINE_URL_STOP_ON_HASH | WINE_URL_STOP_ON_QUESTION;

        switch(parsed_url.nScheme) {
        case URL_SCHEME_FILE:
            int_flags |= WINE_URL_BASH_AS_SLASH | WINE_URL_COLLAPSE_SLASHES | WINE_URL_ESCAPE_HASH;
            int_flags &= ~WINE_URL_STOP_ON_HASH;
            break;

        case URL_SCHEME_HTTP:
        case URL_SCHEME_HTTPS:
            int_flags |= WINE_URL_BASH_AS_SLASH;
            if(parsed_url.pszSuffix[0] != '/' && parsed_url.pszSuffix[0] != '\\')
                int_flags |= WINE_URL_ESCAPE_SLASH;
            break;

        case URL_SCHEME_MAILTO:
            int_flags |= WINE_URL_ESCAPE_SLASH | WINE_URL_ESCAPE_QUESTION | WINE_URL_ESCAPE_HASH;
            int_flags &= ~(WINE_URL_STOP_ON_QUESTION | WINE_URL_STOP_ON_HASH);
            break;

        case URL_SCHEME_INVALID:
            break;

        case URL_SCHEME_FTP:
        default:
            if(parsed_url.pszSuffix[0] != '/')
                int_flags |= WINE_URL_ESCAPE_SLASH;
            break;
        }
    }

    for (src = url; *src; )
    {
        WCHAR cur = *src;
        len = 0;

        if ((int_flags & WINE_URL_COLLAPSE_SLASHES) && src == url + parsed_url.cchProtocol + 1)
        {
            int localhost_len = ARRAY_SIZE(localhost) - 1;
            while (cur == '/' || cur == '\\')
            {
                slashes++;
                cur = *++src;
            }
            if (slashes == 2 && !strncmpiW(src, localhost, localhost_len)) { /* file://localhost/ -> file:/// */
                if(*(src + localhost_len) == '/' || *(src + localhost_len) == '\\')
                src += localhost_len + 1;
                slashes = 3;
            }

            switch (slashes)
            {
            case 1:
            case 3:
                next[0] = next[1] = next[2] = '/';
                len = 3;
                break;
            case 0:
                len = 0;
                break;
            default:
                next[0] = next[1] = '/';
                len = 2;
                break;
            }
        }
        if (len == 0)
        {
            if (cur == '#' && (int_flags & WINE_URL_STOP_ON_HASH))
                stop_escaping = TRUE;

            if (cur == '?' && (int_flags & WINE_URL_STOP_ON_QUESTION))
                stop_escaping = TRUE;

            if (cur == '\\' && (int_flags & WINE_URL_BASH_AS_SLASH) && !stop_escaping) cur = '/';

            if (url_needs_escape(cur, flags, int_flags) && !stop_escaping)
            {
                if (flags & URL_ESCAPE_AS_UTF8)
                {
                    char utf[16];

                    if ((cur >= 0xd800 && cur <= 0xdfff) && (src[1] >= 0xdc00 && src[1] <= 0xdfff))
                    {
                        len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, 2, utf, sizeof(utf), NULL, NULL);
                        src++;
                    }
                    else
                        len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, &cur, 1, utf, sizeof(utf), NULL, NULL);

                    if (!len)
                    {
                        utf[0] = 0xef;
                        utf[1] = 0xbf;
                        utf[2] = 0xbd;
                        len = 3;
                    }

                    for (i = 0; i < len; ++i)
                    {
                        next[i*3+0] = '%';
                        next[i*3+1] = hexDigits[(utf[i] >> 4) & 0xf];
                        next[i*3+2] = hexDigits[utf[i] & 0xf];
                    }
                    len *= 3;
                }
                else
                {
                    next[0] = '%';
                    next[1] = hexDigits[(cur >> 4) & 0xf];
                    next[2] = hexDigits[cur & 0xf];
                    len = 3;
                }
            }
            else
            {
                next[0] = cur;
                len = 1;
            }
            src++;
        }

        if (needed + len <= *escaped_len)
        {
            memcpy(dst, next, len*sizeof(WCHAR));
            dst += len;
        }
        needed += len;
    }

    if (needed < *escaped_len)
    {
        *dst = '\0';
        memcpy(escaped, dst_ptr, (needed+1)*sizeof(WCHAR));
        hr = S_OK;
    }
    else
    {
        needed++; /* add one for the '\0' */
        hr = E_POINTER;
    }
    *escaped_len = needed;

    heap_free(dst_ptr);
    return hr;
}

HRESULT WINAPI UrlCanonicalizeA(const char *src_url, char *canonicalized, DWORD *canonicalized_len, DWORD flags)
{
    LPWSTR url, canonical;
    HRESULT hr;

    TRACE("%s, %p, %p, %#x\n", wine_dbgstr_a(src_url), canonicalized, canonicalized_len, flags);

    if (!src_url || !canonicalized || !canonicalized_len || !*canonicalized_len)
        return E_INVALIDARG;

    url = heap_strdupAtoW(src_url);
    canonical = heap_alloc(*canonicalized_len * sizeof(WCHAR));
    if (!url || !canonical)
    {
        heap_free(url);
        heap_free(canonical);
        return E_OUTOFMEMORY;
    }

    hr = UrlCanonicalizeW(url, canonical, canonicalized_len, flags);
    if (hr == S_OK)
        WideCharToMultiByte(CP_ACP, 0, canonical, -1, canonicalized, *canonicalized_len + 1, NULL, NULL);

    heap_free(url);
    heap_free(canonical);
    return hr;
}

HRESULT WINAPI UrlCanonicalizeW(const WCHAR *src_url, WCHAR *canonicalized, DWORD *canonicalized_len, DWORD flags)
{
    static const WCHAR wszFile[] = {'f','i','l','e',':'};
    static const WCHAR wszRes[] = {'r','e','s',':'};
    static const WCHAR wszHttp[] = {'h','t','t','p',':'};
    static const WCHAR wszLocalhost[] = {'l','o','c','a','l','h','o','s','t'};
    static const WCHAR wszFilePrefix[] = {'f','i','l','e',':','/','/','/'};
    WCHAR *url_copy, *url, *wk2, *mp, *mp2;
    DWORD nByteLen, nLen, nWkLen;
    const WCHAR *wk1, *root;
    DWORD escape_flags;
    WCHAR slash = '\0';
    HRESULT hr = S_OK;
    BOOL is_file_url;
    INT state;

    TRACE("%s, %p, %p, %#x\n", wine_dbgstr_w(src_url), canonicalized, canonicalized_len, flags);

    if (!src_url || !canonicalized || !canonicalized || !*canonicalized_len)
        return E_INVALIDARG;

    if (!*src_url)
    {
        *canonicalized = 0;
        return S_OK;
    }

    /* Remove '\t' characters from URL */
    nByteLen = (strlenW(src_url) + 1) * sizeof(WCHAR); /* length in bytes */
    url = HeapAlloc(GetProcessHeap(), 0, nByteLen);
    if(!url)
        return E_OUTOFMEMORY;

    wk1 = src_url;
    wk2 = url;
    do
    {
        while(*wk1 == '\t')
            wk1++;
        *wk2++ = *wk1;
    } while (*wk1++);

    /* Allocate memory for simplified URL (before escaping) */
    nByteLen = (wk2-url)*sizeof(WCHAR);
    url_copy = heap_alloc(nByteLen + sizeof(wszFilePrefix) + sizeof(WCHAR));
    if (!url_copy)
    {
        heap_free(url);
        return E_OUTOFMEMORY;
    }

    is_file_url = !strncmpW(wszFile, url, ARRAY_SIZE(wszFile));

    if ((nByteLen >= sizeof(wszHttp) && !memcmp(wszHttp, url, sizeof(wszHttp))) || is_file_url)
        slash = '/';

    if ((flags & (URL_FILE_USE_PATHURL | URL_WININET_COMPATIBILITY)) && is_file_url)
        slash = '\\';

    if (nByteLen >= sizeof(wszRes) && !memcmp(wszRes, url, sizeof(wszRes)))
    {
        flags &= ~URL_FILE_USE_PATHURL;
        slash = '\0';
    }

    /*
     * state =
     *         0   initial  1,3
     *         1   have 2[+] alnum  2,3
     *         2   have scheme (found :)  4,6,3
     *         3   failed (no location)
     *         4   have //  5,3
     *         5   have 1[+] alnum  6,3
     *         6   have location (found /) save root location
     */

    wk1 = url;
    wk2 = url_copy;
    state = 0;

    /* Assume path */
    if (url[1] == ':')
    {
        memcpy(wk2, wszFilePrefix, sizeof(wszFilePrefix));
        wk2 += ARRAY_SIZE(wszFilePrefix);
        if (flags & (URL_FILE_USE_PATHURL | URL_WININET_COMPATIBILITY))
        {
            slash = '\\';
            --wk2;
        }
        else
            flags |= URL_ESCAPE_UNSAFE;
        state = 5;
        is_file_url = TRUE;
    }
    else if (url[0] == '/')
    {
        state = 5;
        is_file_url = TRUE;
    }

    while (*wk1)
    {
        switch (state)
        {
        case 0:
            if (!isalnumW(*wk1)) {state = 3; break;}
            *wk2++ = *wk1++;
            if (!isalnumW(*wk1)) {state = 3; break;}
            *wk2++ = *wk1++;
            state = 1;
            break;
        case 1:
            *wk2++ = *wk1;
            if (*wk1++ == ':') state = 2;
            break;
        case 2:
            *wk2++ = *wk1++;
            if (*wk1 != '/') {state = 6; break;}
            *wk2++ = *wk1++;
            if ((flags & URL_FILE_USE_PATHURL) && nByteLen >= sizeof(wszLocalhost) && is_file_url
                    && !memcmp(wszLocalhost, wk1, sizeof(wszLocalhost)))
            {
                wk1 += ARRAY_SIZE(wszLocalhost);
                while (*wk1 == '\\' && (flags & URL_FILE_USE_PATHURL))
                    wk1++;
            }

            if (*wk1 == '/' && (flags & URL_FILE_USE_PATHURL))
                wk1++;
            else if (is_file_url)
            {
                const WCHAR *body = wk1;

                while (*body == '/')
                    ++body;

                if (isalnumW(*body) && *(body+1) == ':')
                {
                    if (!(flags & (URL_WININET_COMPATIBILITY | URL_FILE_USE_PATHURL)))
                    {
                        if (slash)
                            *wk2++ = slash;
                        else
                            *wk2++ = '/';
                    }
                }
                else
                {
                    if (flags & URL_WININET_COMPATIBILITY)
                    {
                        if (*wk1 == '/' && *(wk1 + 1) != '/')
                        {
                            *wk2++ = '\\';
                        }
                        else
                        {
                            *wk2++ = '\\';
                            *wk2++ = '\\';
                        }
                    }
                    else
                    {
                        if (*wk1 == '/' && *(wk1+1) != '/')
                        {
                            if (slash)
                                *wk2++ = slash;
                            else
                                *wk2++ = '/';
                        }
                    }
                }
                wk1 = body;
            }
            state = 4;
            break;
        case 3:
            nWkLen = strlenW(wk1);
            memcpy(wk2, wk1, (nWkLen + 1) * sizeof(WCHAR));
            mp = wk2;
            wk1 += nWkLen;
            wk2 += nWkLen;

            if (slash)
            {
                while (mp < wk2)
                {
                    if (*mp == '/' || *mp == '\\')
                        *mp = slash;
                    mp++;
                }
            }
            break;
        case 4:
            if (!isalnumW(*wk1) && (*wk1 != '-') && (*wk1 != '.') && (*wk1 != ':'))
            {
                state = 3;
                break;
            }
            while (isalnumW(*wk1) || (*wk1 == '-') || (*wk1 == '.') || (*wk1 == ':'))
                *wk2++ = *wk1++;
            state = 5;
            if (!*wk1)
            {
                if (slash)
                    *wk2++ = slash;
                else
                    *wk2++ = '/';
            }
            break;
        case 5:
            if (*wk1 != '/' && *wk1 != '\\')
            {
                state = 3;
                break;
            }
            while (*wk1 == '/' || *wk1 == '\\')
            {
                if (slash)
                    *wk2++ = slash;
                else
                    *wk2++ = *wk1;
                wk1++;
            }
            state = 6;
            break;
        case 6:
            if (flags & URL_DONT_SIMPLIFY)
            {
                state = 3;
                break;
            }

            /* Now at root location, cannot back up any more. */
            /* "root" will point at the '/' */

            root = wk2-1;
            while (*wk1)
            {
                mp = strchrW(wk1, '/');
                mp2 = strchrW(wk1, '\\');
                if (mp2 && (!mp || mp2 < mp))
                    mp = mp2;
                if (!mp)
                {
                    nWkLen = strlenW(wk1);
                    memcpy(wk2, wk1, (nWkLen + 1) * sizeof(WCHAR));
                    wk1 += nWkLen;
                    wk2 += nWkLen;
                    continue;
                }
                nLen = mp - wk1;
                if (nLen)
                {
                    memcpy(wk2, wk1, nLen * sizeof(WCHAR));
                    wk2 += nLen;
                    wk1 += nLen;
                }
                if (slash)
                    *wk2++ = slash;
                else
                    *wk2++ = *wk1;
                wk1++;

                while (*wk1 == '.')
                {
                    TRACE("found '/.'\n");
                    if (wk1[1] == '/' || wk1[1] == '\\')
                    {
                        /* case of /./ -> skip the ./ */
                        wk1 += 2;
                    }
                    else if (wk1[1] == '.' && (wk1[2] == '/' || wk1[2] == '\\' || wk1[2] == '?'
                            || wk1[2] == '#' || !wk1[2]))
                    {
                        /* case /../ -> need to backup wk2 */
                        TRACE("found '/../'\n");
                        *(wk2-1) = '\0';  /* set end of string */
                        mp = strrchrW(root, '/');
                        mp2 = strrchrW(root, '\\');
                        if (mp2 && (!mp || mp2 < mp))
                            mp = mp2;
                        if (mp && (mp >= root))
                        {
                            /* found valid backup point */
                            wk2 = mp + 1;
                            if(wk1[2] != '/' && wk1[2] != '\\')
                                wk1 += 2;
                            else
                                wk1 += 3;
                        }
                        else
                        {
                            /* did not find point, restore '/' */
                            *(wk2-1) = slash;
                            break;
                        }
                    }
                    else
                        break;
                }
            }
            *wk2 = '\0';
            break;
        default:
            FIXME("how did we get here - state=%d\n", state);
            heap_free(url_copy);
            heap_free(url);
            return E_INVALIDARG;
        }
        *wk2 = '\0';
        TRACE("Simplified, orig <%s>, simple <%s>\n", wine_dbgstr_w(src_url), wine_dbgstr_w(url_copy));
    }
    nLen = lstrlenW(url_copy);
    while ((nLen > 0) && ((url_copy[nLen-1] <= ' ')))
        url_copy[--nLen]=0;

    if ((flags & URL_UNESCAPE) || ((flags & URL_FILE_USE_PATHURL) && nByteLen >= sizeof(wszFile)
                && !memcmp(wszFile, url, sizeof(wszFile))))
    {
        UrlUnescapeW(url_copy, NULL, &nLen, URL_UNESCAPE_INPLACE);
    }

    escape_flags = flags & (URL_ESCAPE_UNSAFE | URL_ESCAPE_SPACES_ONLY | URL_ESCAPE_PERCENT |
            URL_DONT_ESCAPE_EXTRA_INFO | URL_ESCAPE_SEGMENT_ONLY);

    if (escape_flags)
    {
        escape_flags &= ~URL_ESCAPE_UNSAFE;
        hr = UrlEscapeW(url_copy, canonicalized, canonicalized_len, escape_flags);
    }
    else
    {
        /* No escaping needed, just copy the string */
        nLen = lstrlenW(url_copy);
        if (nLen < *canonicalized_len)
            memcpy(canonicalized, url_copy, (nLen + 1)*sizeof(WCHAR));
        else
        {
            hr = E_POINTER;
            nLen++;
        }
        *canonicalized_len = nLen;
    }

    heap_free(url_copy);
    heap_free(url);

    if (hr == S_OK)
        TRACE("result %s\n", wine_dbgstr_w(canonicalized));

    return hr;
}

HRESULT WINAPI UrlApplySchemeA(const char *url, char *out, DWORD *out_len, DWORD flags)
{
    LPWSTR inW, outW;
    HRESULT hr;
    DWORD len;

    TRACE("%s, %p, %p:out size %d, %#x\n", wine_dbgstr_a(url), out, out_len, out_len ? *out_len : 0, flags);

    if (!url || !out || !out_len)
        return E_INVALIDARG;

    inW = heap_alloc(2 * INTERNET_MAX_URL_LENGTH * sizeof(WCHAR));
    outW = inW + INTERNET_MAX_URL_LENGTH;

    MultiByteToWideChar(CP_ACP, 0, url, -1, inW, INTERNET_MAX_URL_LENGTH);
    len = INTERNET_MAX_URL_LENGTH;

    hr = UrlApplySchemeW(inW, outW, &len, flags);
    if (hr != S_OK)
    {
        heap_free(inW);
        return hr;
    }

    len = WideCharToMultiByte(CP_ACP, 0, outW, -1, NULL, 0, NULL, NULL);
    if (len > *out_len)
    {
        hr = E_POINTER;
        goto cleanup;
    }

    WideCharToMultiByte(CP_ACP, 0, outW, -1, out, *out_len, NULL, NULL);
    len--;

cleanup:
    *out_len = len;
    heap_free(inW);
    return hr;
}

static HRESULT url_guess_scheme(const WCHAR *url, WCHAR *out, DWORD *out_len)
{
    WCHAR reg_path[MAX_PATH], value[MAX_PATH], data[MAX_PATH];
    DWORD value_len, data_len, dwType, i;
    WCHAR Wxx, Wyy;
    HKEY newkey;
    INT index;
    BOOL j;

    MultiByteToWideChar(CP_ACP, 0,
            "Software\\Microsoft\\Windows\\CurrentVersion\\URL\\Prefixes", 1, reg_path, MAX_PATH);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_path, 0, 1, &newkey);
    index = 0;
    while (value_len = data_len = MAX_PATH,
            RegEnumValueW(newkey, index, value, &value_len, 0, &dwType, (LPVOID)data, &data_len) == 0)
    {
        TRACE("guess %d %s is %s\n", index, wine_dbgstr_w(value), wine_dbgstr_w(data));

        j = FALSE;
        for (i = 0; i < value_len; ++i)
        {
            Wxx = url[i];
            Wyy = value[i];
            /* remember that TRUE is not-equal */
            j = ChrCmpIW(Wxx, Wyy);
            if (j) break;
        }
        if ((i == value_len) && !j)
        {
            if (strlenW(data) + strlenW(url) + 1 > *out_len)
            {
                *out_len = strlenW(data) + strlenW(url) + 1;
                RegCloseKey(newkey);
                return E_POINTER;
            }
            strcpyW(out, data);
            strcatW(out, url);
            *out_len = strlenW(out);
            TRACE("matched and set to %s\n", wine_dbgstr_w(out));
            RegCloseKey(newkey);
            return S_OK;
        }
        index++;
    }
    RegCloseKey(newkey);
    return E_FAIL;
}

static HRESULT url_create_from_path(const WCHAR *path, WCHAR *url, DWORD *url_len)
{
    static const WCHAR file_colonW[] = {'f','i','l','e',':',0};
    static const WCHAR three_slashesW[] = {'/','/','/',0};
    PARSEDURLW parsed_url;
    WCHAR *new_url;
    DWORD needed;
    HRESULT hr;

    parsed_url.cbSize = sizeof(parsed_url);
    if (ParseURLW(path, &parsed_url) == S_OK)
    {
        if (parsed_url.nScheme != URL_SCHEME_INVALID && parsed_url.cchProtocol > 1)
        {
            needed = strlenW(path);
            if (needed >= *url_len)
            {
                *url_len = needed + 1;
                return E_POINTER;
            }
            else
            {
                *url_len = needed;
                return S_FALSE;
            }
        }
    }

    new_url = heap_alloc((strlenW(path) + 9) * sizeof(WCHAR)); /* "file:///" + path length + 1 */
    strcpyW(new_url, file_colonW);
    if (isalphaW(path[0]) && path[1] == ':')
        strcatW(new_url, three_slashesW);
    strcatW(new_url, path);
    hr = UrlEscapeW(new_url, url, url_len, URL_ESCAPE_PERCENT);
    heap_free(new_url);
    return hr;
}

static HRESULT url_apply_default_scheme(const WCHAR *url, WCHAR *out, DWORD *length)
{
    static const WCHAR prefix_keyW[] =
        {'S','o','f','t','w','a','r','e',
         '\\','M','i','c','r','o','s','o','f','t',
         '\\','W','i','n','d','o','w','s',
         '\\','C','u','r','r','e','n','t','V','e','r','s','i','o','n',
         '\\','U','R','L',
         '\\','D','e','f','a','u','l','t','P','r','e','f','i','x',0};
    DWORD data_len, dwType;
    WCHAR data[MAX_PATH];
    HKEY newkey;

    /* get and prepend default */
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, prefix_keyW, 0, 1, &newkey);
    data_len = sizeof(data);
    RegQueryValueExW(newkey, NULL, 0, &dwType, (BYTE *)data, &data_len);
    RegCloseKey(newkey);
    if (strlenW(data) + strlenW(url) + 1 > *length)
    {
        *length = strlenW(data) + strlenW(url) + 1;
        return E_POINTER;
    }
    strcpyW(out, data);
    strcatW(out, url);
    *length = strlenW(out);
    TRACE("used default %s\n", wine_dbgstr_w(out));
    return S_OK;
}

HRESULT WINAPI UrlApplySchemeW(const WCHAR *url, WCHAR *out, DWORD *length, DWORD flags)
{
    PARSEDURLW in_scheme;
    DWORD res1;
    HRESULT hr;

    TRACE("%s, %p, %p:out size %d, %#x\n", wine_dbgstr_w(url), out, length, length ? *length : 0, flags);

    if (!url || !out || !length)
        return E_INVALIDARG;

    if (flags & URL_APPLY_GUESSFILE)
    {
        if (*length > 1 && ':' == url[1])
        {
            res1 = *length;
            hr = url_create_from_path(url, out, &res1);
            if (hr == S_OK || hr == E_POINTER)
            {
                *length = res1;
                return hr;
            }
            else if (hr == S_FALSE)
            {
                return hr;
            }
        }
    }

    in_scheme.cbSize = sizeof(in_scheme);
    /* See if the base has a scheme */
    res1 = ParseURLW(url, &in_scheme);
    if (res1)
    {
        /* no scheme in input, need to see if we need to guess */
        if (flags & URL_APPLY_GUESSSCHEME)
        {
            if ((hr = url_guess_scheme(url, out, length)) != E_FAIL)
                return hr;
        }
    }

    /* If we are here, then either invalid scheme,
     * or no scheme and can't/failed guess.
     */
    if ((((res1 == 0) && (flags & URL_APPLY_FORCEAPPLY)) || ((res1 != 0)) ) && (flags & URL_APPLY_DEFAULT))
        return url_apply_default_scheme(url, out, length);

    return S_FALSE;
}

INT WINAPI UrlCompareA(const char *url1, const char *url2, BOOL ignore_slash)
{
    INT ret, len, len1, len2;

    if (!ignore_slash)
        return strcmp(url1, url2);
    len1 = strlen(url1);
    if (url1[len1-1] == '/') len1--;
    len2 = strlen(url2);
    if (url2[len2-1] == '/') len2--;
    if (len1 == len2)
        return strncmp(url1, url2, len1);
    len = min(len1, len2);
    ret = strncmp(url1, url2, len);
    if (ret) return ret;
    if (len1 > len2) return 1;
    return -1;
}

INT WINAPI UrlCompareW(const WCHAR *url1, const WCHAR *url2, BOOL ignore_slash)
{
    size_t len, len1, len2;
    INT ret;

    if (!ignore_slash)
        return strcmpW(url1, url2);
    len1 = strlenW(url1);
    if (url1[len1-1] == '/') len1--;
    len2 = strlenW(url2);
    if (url2[len2-1] == '/') len2--;
    if (len1 == len2)
        return strncmpW(url1, url2, len1);
    len = min(len1, len2);
    ret = strncmpW(url1, url2, len);
    if (ret) return ret;
    if (len1 > len2) return 1;
    return -1;
}

HRESULT WINAPI UrlFixupW(const WCHAR *url, WCHAR *translatedUrl, DWORD maxChars)
{
    DWORD srcLen;

    FIXME("%s, %p, %d stub\n", wine_dbgstr_w(url), translatedUrl, maxChars);

    if (!url)
        return E_FAIL;

    srcLen = lstrlenW(url) + 1;

    /* For now just copy the URL directly */
    lstrcpynW(translatedUrl, url, (maxChars < srcLen) ? maxChars : srcLen);

    return S_OK;
}

const char * WINAPI UrlGetLocationA(const char *url)
{
    PARSEDURLA base;

    base.cbSize = sizeof(base);
    if (ParseURLA(url, &base) != S_OK) return NULL;  /* invalid scheme */

    /* if scheme is file: then never return pointer */
    if (!strncmp(base.pszProtocol, "file", min(4, base.cchProtocol)))
        return NULL;

    /* Look for '#' and return its addr */
    return strchr(base.pszSuffix, '#');
}

const WCHAR * WINAPI UrlGetLocationW(const WCHAR *url)
{
    static const WCHAR fileW[] = {'f','i','l','e','\0'};
    PARSEDURLW base;

    base.cbSize = sizeof(base);
    if (ParseURLW(url, &base) != S_OK) return NULL;  /* invalid scheme */

    /* if scheme is file: then never return pointer */
    if (!strncmpW(base.pszProtocol, fileW, min(4, base.cchProtocol)))
        return NULL;

    /* Look for '#' and return its addr */
    return strchrW(base.pszSuffix, '#');
}

HRESULT WINAPI UrlGetPartA(const char *url, char *out, DWORD *out_len, DWORD part, DWORD flags)
{
    LPWSTR inW, outW;
    DWORD len, len2;
    HRESULT hr;

    if (!url || !out || !out_len || !*out_len)
        return E_INVALIDARG;

    inW = heap_alloc(2 * INTERNET_MAX_URL_LENGTH * sizeof(WCHAR));
    outW = inW + INTERNET_MAX_URL_LENGTH;

    MultiByteToWideChar(CP_ACP, 0, url, -1, inW, INTERNET_MAX_URL_LENGTH);

    len = INTERNET_MAX_URL_LENGTH;
    hr = UrlGetPartW(inW, outW, &len, part, flags);
    if (FAILED(hr))
    {
        heap_free(inW);
        return hr;
    }

    len2 = WideCharToMultiByte(CP_ACP, 0, outW, len, NULL, 0, NULL, NULL);
    if (len2 > *out_len)
    {
        *out_len = len2 + 1;
        heap_free(inW);
        return E_POINTER;
    }
    len2 = WideCharToMultiByte(CP_ACP, 0, outW, len + 1, out, *out_len, NULL, NULL);
    *out_len = len2 - 1;
    heap_free(inW);
    return hr;
}

static const WCHAR * scan_url(const WCHAR *start, DWORD *size, enum url_scan_type type)
{
    static DWORD alwayszero = 0;
    BOOL cont = TRUE;

    *size = 0;

    switch (type)
    {
    case SCHEME:
        while (cont)
        {
            if ((islowerW(*start) && isalphaW(*start)) ||
                    isdigitW(*start) || *start == '+' || *start == '-' || *start == '.')
            {
                start++;
                (*size)++;
            }
            else
                cont = FALSE;
        }
        if (*start != ':')
            *size = 0;
        break;

    case USERPASS:
        while (cont)
        {
            if (isalphaW(*start) ||
                    isdigitW(*start) ||
                    /* user/password only characters */
                    (*start == ';') ||
                    (*start == '?') ||
                    (*start == '&') ||
                    (*start == '=') ||
                    /* *extra* characters */
                    (*start == '!') ||
                    (*start == '*') ||
                    (*start == '\'') ||
                    (*start == '(') ||
                    (*start == ')') ||
                    (*start == ',') ||
                    /* *safe* characters */
                    (*start == '$') ||
                    (*start == '_') ||
                    (*start == '+') ||
                    (*start == '-') ||
                    (*start == '.') ||
                    (*start == ' '))
            {
                start++;
                (*size)++;
            }
            else if (*start == '%')
            {
                if (isxdigitW(*(start + 1)) && isxdigitW(*(start + 2)))
                {
                    start += 3;
                    *size += 3;
                }
                else
                    cont = FALSE;
            } else
                cont = FALSE;
        }
        break;

    case PORT:
        while (cont)
        {
            if (isdigitW(*start))
            {
                start++;
                (*size)++;
            }
            else
                cont = FALSE;
        }
        break;

    case HOST:
        while (cont)
        {
            if (isalnumW(*start) || *start == '-' || *start == '.' || *start == ' ' || *start == '*')
            {
                start++;
                (*size)++;
            }
            else
                cont = FALSE;
        }
        break;

    default:
        FIXME("unknown type %d\n", type);
        return (LPWSTR)&alwayszero;
    }

    return start;
}

static LONG parse_url(const WCHAR *url, struct parsed_url *pl)
{
    const WCHAR *work;

    memset(pl, 0, sizeof(*pl));
    pl->scheme = url;
    work = scan_url(pl->scheme, &pl->scheme_len, SCHEME);
    if (!*work || (*work != ':')) goto ErrorExit;
    work++;
    if ((*work != '/') || (*(work+1) != '/')) goto SuccessExit;

    pl->username = work + 2;
    work = scan_url(pl->username, &pl->username_len, USERPASS);
    if (*work == ':' )
    {
        /* parse password */
        work++;
        pl->password = work;
        work = scan_url(pl->password, &pl->password_len, USERPASS);
        if (*work != '@')
        {
            /* what we just parsed must be the hostname and port
             * so reset pointers and clear then let it parse */
            pl->username_len = pl->password_len = 0;
            work = pl->username - 1;
            pl->username = pl->password = 0;
        }
    }
    else if (*work == '@')
    {
        /* no password */
        pl->password_len = 0;
        pl->password = 0;
    }
    else if (!*work || *work == '/' || *work == '.')
    {
        /* what was parsed was hostname, so reset pointers and let it parse */
        pl->username_len = pl->password_len = 0;
        work = pl->username - 1;
        pl->username = pl->password = 0;
    }
    else goto ErrorExit;

    /* now start parsing hostname or hostnumber */
    work++;
    pl->hostname = work;
    work = scan_url(pl->hostname, &pl->hostname_len, HOST);
    if (*work == ':')
    {
        /* parse port */
        work++;
        pl->port = work;
        work = scan_url(pl->port, &pl->port_len, PORT);
    }
    if (*work == '/')
    {
        /* see if query string */
        pl->query = strchrW(work, '?');
        if (pl->query) pl->query_len = strlenW(pl->query);
    }
  SuccessExit:
    TRACE("parse successful: scheme=%p(%d), user=%p(%d), pass=%p(%d), host=%p(%d), port=%p(%d), query=%p(%d)\n",
            pl->scheme, pl->scheme_len, pl->username, pl->username_len, pl->password, pl->password_len, pl->hostname,
            pl->hostname_len, pl->port, pl->port_len, pl->query, pl->query_len);

    return S_OK;

  ErrorExit:
    FIXME("failed to parse %s\n", debugstr_w(url));
    return E_INVALIDARG;
}

HRESULT WINAPI UrlGetPartW(const WCHAR *url, WCHAR *out, DWORD *out_len, DWORD part, DWORD flags)
{
    DWORD scheme, size, schsize;
    LPCWSTR addr, schaddr;
    struct parsed_url pl;
    HRESULT hr;

    TRACE("%s, %p, %p(%d), %#x, %#x\n", wine_dbgstr_w(url), out, out_len, *out_len, part, flags);

    if (!url || !out || !out_len || !*out_len)
        return E_INVALIDARG;

    *out = '\0';

    addr = strchrW(url, ':');
    if (!addr)
        scheme = URL_SCHEME_UNKNOWN;
    else
        scheme = get_scheme_code(url, addr - url);

    hr = parse_url(url, &pl);

    switch (part)
    {
    case URL_PART_SCHEME:
        if (!pl.scheme_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.scheme;
        size = pl.scheme_len;
        break;

    case URL_PART_HOSTNAME:
        switch (scheme)
        {
            case URL_SCHEME_FTP:
            case URL_SCHEME_HTTP:
            case URL_SCHEME_GOPHER:
            case URL_SCHEME_TELNET:
            case URL_SCHEME_FILE:
            case URL_SCHEME_HTTPS:
                break;
            default:
                *out_len = 0;
                return E_FAIL;
        }

        if (scheme == URL_SCHEME_FILE && (!pl.hostname_len || (pl.hostname_len == 1 && *(pl.hostname + 1) == ':')))
        {
            *out_len = 0;
            return S_FALSE;
        }

        if (!pl.hostname_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.hostname;
        size = pl.hostname_len;
        break;

    case URL_PART_USERNAME:
        if (!pl.username_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.username;
        size = pl.username_len;
        break;

    case URL_PART_PASSWORD:
        if (!pl.password_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.password;
        size = pl.password_len;
        break;

    case URL_PART_PORT:
        if (!pl.port_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.port;
        size = pl.port_len;
        break;

    case URL_PART_QUERY:
        if (!pl.query_len)
        {
            *out_len = 0;
            return S_FALSE;
        }
        addr = pl.query;
        size = pl.query_len;
        break;

    default:
        *out_len = 0;
        return E_INVALIDARG;
    }

    if (flags == URL_PARTFLAG_KEEPSCHEME)
    {
        if (!pl.scheme || !pl.scheme_len)
        {
            *out_len = 0;
            return E_FAIL;
        }
        schaddr = pl.scheme;
        schsize = pl.scheme_len;
        if (*out_len < schsize + size + 2)
        {
            *out_len = schsize + size + 2;
            return E_POINTER;
        }
        memcpy(out, schaddr, schsize*sizeof(WCHAR));
        out[schsize] = ':';
        memcpy(out + schsize+1, addr, size*sizeof(WCHAR));
        out[schsize+1+size] = 0;
        *out_len = schsize + 1 + size;
    }
    else
    {
        if (*out_len < size + 1)
        {
            *out_len = size + 1;
            return E_POINTER;
        }
        memcpy(out, addr, size*sizeof(WCHAR));
        out[size] = 0;
        *out_len = size;
    }
    TRACE("len=%d %s\n", *out_len, wine_dbgstr_w(out));

    return hr;
}

BOOL WINAPI UrlIsA(const char *url, URLIS Urlis)
{
    const char *last;
    PARSEDURLA base;

    TRACE("%s, %d\n", debugstr_a(url), Urlis);

    if (!url)
        return FALSE;

    switch (Urlis) {

    case URLIS_OPAQUE:
        base.cbSize = sizeof(base);
        if (ParseURLA(url, &base) != S_OK) return FALSE;  /* invalid scheme */
        switch (base.nScheme)
        {
        case URL_SCHEME_MAILTO:
        case URL_SCHEME_SHELL:
        case URL_SCHEME_JAVASCRIPT:
        case URL_SCHEME_VBSCRIPT:
        case URL_SCHEME_ABOUT:
            return TRUE;
        }
        return FALSE;

    case URLIS_FILEURL:
        return (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, url, 5, "file:", 5) == CSTR_EQUAL);

    case URLIS_DIRECTORY:
        last = url + strlen(url) - 1;
        return (last >= url && (*last == '/' || *last == '\\' ));

    case URLIS_URL:
        return PathIsURLA(url);

    case URLIS_NOHISTORY:
    case URLIS_APPLIABLE:
    case URLIS_HASQUERY:
    default:
        FIXME("(%s %d): stub\n", debugstr_a(url), Urlis);
    }

    return FALSE;
}

BOOL WINAPI UrlIsW(const WCHAR *url, URLIS Urlis)
{
    static const WCHAR file_colon[] = {'f','i','l','e',':',0};
    const WCHAR *last;
    PARSEDURLW base;

    TRACE("%s, %d\n", debugstr_w(url), Urlis);

    if (!url)
        return FALSE;

    switch (Urlis)
    {
    case URLIS_OPAQUE:
        base.cbSize = sizeof(base);
        if (ParseURLW(url, &base) != S_OK) return FALSE;  /* invalid scheme */
        switch (base.nScheme)
        {
        case URL_SCHEME_MAILTO:
        case URL_SCHEME_SHELL:
        case URL_SCHEME_JAVASCRIPT:
        case URL_SCHEME_VBSCRIPT:
        case URL_SCHEME_ABOUT:
            return TRUE;
        }
        return FALSE;

    case URLIS_FILEURL:
        return (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, url, 5, file_colon, 5) == CSTR_EQUAL);

    case URLIS_DIRECTORY:
        last = url + strlenW(url) - 1;
        return (last >= url && (*last == '/' || *last == '\\'));

    case URLIS_URL:
        return PathIsURLW(url);

    case URLIS_NOHISTORY:
    case URLIS_APPLIABLE:
    case URLIS_HASQUERY:
    default:
        FIXME("(%s %d): stub\n", debugstr_w(url), Urlis);
    }

    return FALSE;
}

BOOL WINAPI UrlIsOpaqueA(const char *url)
{
    return UrlIsA(url, URLIS_OPAQUE);
}

BOOL WINAPI UrlIsOpaqueW(const WCHAR *url)
{
    return UrlIsW(url, URLIS_OPAQUE);
}

BOOL WINAPI UrlIsNoHistoryA(const char *url)
{
    return UrlIsA(url, URLIS_NOHISTORY);
}

BOOL WINAPI UrlIsNoHistoryW(const WCHAR *url)
{
    return UrlIsW(url, URLIS_NOHISTORY);
}

HRESULT WINAPI UrlCreateFromPathA(const char *path, char *url, DWORD *url_len, DWORD reserved)
{
    WCHAR bufW[INTERNET_MAX_URL_LENGTH];
    DWORD lenW = ARRAY_SIZE(bufW), lenA;
    UNICODE_STRING pathW;
    WCHAR *urlW = bufW;
    HRESULT hr;

    if (!RtlCreateUnicodeStringFromAsciiz(&pathW, path))
        return E_INVALIDARG;

    if ((hr = UrlCreateFromPathW(pathW.Buffer, urlW, &lenW, reserved)) == E_POINTER)
    {
        urlW = heap_alloc(lenW * sizeof(WCHAR));
        hr = UrlCreateFromPathW(pathW.Buffer, urlW, &lenW, reserved);
    }

    if (SUCCEEDED(hr))
    {
        RtlUnicodeToMultiByteSize(&lenA, urlW, lenW * sizeof(WCHAR));
        if (*url_len > lenA)
        {
            RtlUnicodeToMultiByteN(url, *url_len - 1, &lenA, urlW, lenW * sizeof(WCHAR));
            url[lenA] = 0;
            *url_len = lenA;
        }
        else
        {
            *url_len = lenA + 1;
            hr = E_POINTER;
        }
    }
    if (urlW != bufW)
        heap_free(urlW);
    RtlFreeUnicodeString(&pathW);
    return hr;
}

HRESULT WINAPI UrlCreateFromPathW(const WCHAR *path, WCHAR *url, DWORD *url_len, DWORD reserved)
{
    HRESULT hr;

    TRACE("%s, %p, %p, %#x\n", debugstr_w(path), url, url_len, reserved);

    if (reserved || !url || !url_len)
        return E_INVALIDARG;

    hr = url_create_from_path(path, url, url_len);
    if (hr == S_FALSE)
        strcpyW(url, path);

    return hr;
}
