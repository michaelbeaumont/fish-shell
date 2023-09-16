// Functions for setting and getting environment variables.
#include "config.h"  // IWYU pragma: keep

#include "env.h"

#include <pwd.h>
#include <unistd.h>

#include "history.h"
#include "reader.h"

/// At init, we read all the environment variables from this array.
extern char **environ;

#if defined(__APPLE__) || defined(__CYGWIN__)
static int check_runtime_path(const char *path) {
    UNUSED(path);
    return 0;
}
#else
/// Check, and create if necessary, a secure runtime path. Derived from tmux.c in tmux
/// (http://tmux.sourceforge.net/).
static int check_runtime_path(const char *path) {
    // Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
    //
    // Permission to use, copy, modify, and distribute this software for any
    // purpose with or without fee is hereby granted, provided that the above
    // copyright notice and this permission notice appear in all copies.
    //
    // THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    // WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    // MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    // ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    // WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
    // IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
    // OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
    struct stat statpath;
    uid_t uid = geteuid();

    if (mkdir(path, S_IRWXU) != 0 && errno != EEXIST) return errno;
    if (lstat(path, &statpath) != 0) return errno;
    if (!S_ISDIR(statpath.st_mode) || statpath.st_uid != uid ||
        (statpath.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return EACCES;
    return 0;
}
#endif

/// Return the path of an appropriate runtime data directory.
wcstring env_get_runtime_path() {
    wcstring result;
    const char *dir = getenv("XDG_RUNTIME_DIR");

    // Check that the path is actually usable. Technically this is guaranteed by the fdo spec but in
    // practice it is not always the case: see #1828 and #2222.
    if (dir != nullptr && check_runtime_path(dir) == 0) {
        result = str2wcstring(dir);
    } else {
        // Don't rely on $USER being set, as setup_user() has not yet been called.
        // See https://github.com/fish-shell/fish-shell/issues/5180
        // getpeuid() can't fail, but getpwuid sure can.
        auto pwuid = getpwuid(geteuid());
        const char *uname = pwuid ? pwuid->pw_name : nullptr;
        // /tmp/fish.user
        std::string tmpdir = get_path_to_tmp_dir() + "/fish.";
        if (uname) {
            tmpdir.append(uname);
        }

        if (!uname || check_runtime_path(tmpdir.c_str()) != 0) {
            FLOG(error, L"Runtime path not available.");
            FLOGF(error, L"Try deleting the directory %s and restarting fish.", tmpdir.c_str());
            return result;
        }

        result = str2wcstring(tmpdir);
    }
    return result;
}

static std::mutex s_setenv_lock{};

extern "C" {
void setenv_lock(const char *name, const char *value, int overwrite) {
    scoped_lock locker(s_setenv_lock);
    setenv(name, value, overwrite);
}

void unsetenv_lock(const char *name) {
    scoped_lock locker(s_setenv_lock);
    unsetenv(name);
}
}

static std::map<wcstring, wcstring> inheriteds;

const std::map<wcstring, wcstring> &env_get_inherited() { return inheriteds; }

void set_inheriteds_ffi() {
    wcstring key, val;
    const char *const *envp = environ;
    int i = 0;
    while (envp && envp[i]) i++;
    while (i--) {
        const wcstring key_and_val = str2wcstring(envp[i]);
        size_t eql = key_and_val.find(L'=');
        if (eql == wcstring::npos) {
            // PORTING: Should this not be key_and_val?
            inheriteds[key] = L"";
        } else {
            key.assign(key_and_val, 0, eql);
            val.assign(key_and_val, eql + 1, wcstring::npos);
            inheriteds[key] = val;
        }
    }
}

wcstring_list_ffi_t get_history_variable_text_ffi(const wcstring &fish_history_val) {
    wcstring_list_ffi_t out{};
    maybe_t<rust::Box<HistorySharedPtr>> history = commandline_get_state().history;
    if (!history) {
        // Effective duplication of history_session_id().
        wcstring session_id{};
        if (fish_history_val.empty()) {
            // No session.
            session_id.clear();
        } else if (!valid_var_name(fish_history_val)) {
            session_id = L"fish";
            FLOGF(error,
                  _(L"History session ID '%ls' is not a valid variable name. "
                    L"Falling back to `%ls`."),
                  fish_history_val.c_str(), session_id.c_str());
        } else {
            // Valid session.
            session_id = fish_history_val;
        }
        history = history_with_name(session_id);
    }
    if (history) {
        out = *(*history)->get_history();
    }
    return out;
}
