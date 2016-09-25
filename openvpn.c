/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2004 Mathias Sundman <mathias@nilings.se>
 *                2010 Heiko Hund <heikoh@users.sf.net>
 *                2016 Selva Nair <selva.nair@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <process.h>
#include <richedit.h>
#include <time.h>
#include <string.h>

#include "tray.h"
#include "main.h"
#include "openvpn.h"
#include "openvpn_config.h"
#include "openvpn-gui-res.h"
#include "options.h"
#include "scripts.h"
#include "viewlog.h"
#include "proxy.h"
#include "passphrase.h"
#include "localization.h"
#include "misc.h"
#include "access.h"
#include "save_pass.h"

#define WM_OVPN_STOP    (WM_APP + 10)
#define WM_OVPN_SUSPEND (WM_APP + 11)

extern options_t o;

const TCHAR *cfgProp = _T("conn");

typedef struct {
    connection_t *c;
    int challenge_echo;
    char *challenge_str;
} auth_param_t;

/*
 * Receive banner on connection to management interface
 * Format: <BANNER>
 */
void
OnReady(connection_t *c, UNUSED char *msg)
{
    ManagementCommand(c, "state on", NULL, regular);
    ManagementCommand(c, "log all on", OnLogLine, combined);
}


/*
 * Handle the request to release a hold from the OpenVPN management interface
 */
void
OnHold(connection_t *c, UNUSED char *msg)
{
    ManagementCommand(c, "hold off", NULL, regular);
    ManagementCommand(c, "hold release", NULL, regular);
}

/*
 * Handle a log line from the OpenVPN management interface
 * Format <TIMESTAMP>,<FLAGS>,<MESSAGE>
 */
void
OnLogLine(connection_t *c, char *line)
{
    HWND logWnd = GetDlgItem(c->hwndStatus, ID_EDT_LOG);
    char *flags, *message;
    time_t timestamp;
    TCHAR *datetime;
    const SETTEXTEX ste = {
        .flags = ST_SELECTION,
        .codepage = CP_UTF8
    };

    flags = strchr(line, ',') + 1;
    if (flags - 1 == NULL)
        return;

    message = strchr(flags, ',') + 1;
    if (message - 1 == NULL)
        return;

    /* Remove lines from log window if it is getting full */
    if (SendMessage(logWnd, EM_GETLINECOUNT, 0, 0) > MAX_LOG_LINES)
    {
        int pos = SendMessage(logWnd, EM_LINEINDEX, DEL_LOG_LINES, 0);
        SendMessage(logWnd, EM_SETSEL, 0, pos);
        SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T(""));
    }

    timestamp = strtol(line, NULL, 10);
    datetime = _tctime(&timestamp);
    datetime[24] = _T(' ');

    /* Append line to log window */
    SendMessage(logWnd, EM_SETSEL, (WPARAM) -1, (LPARAM) -1);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) datetime);
    SendMessage(logWnd, EM_SETTEXTEX, (WPARAM) &ste, (LPARAM) message);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T("\n"));
}


/*
 * Handle a state change notification from the OpenVPN management interface
 * Format <TIMESTAMP>,<STATE>,[<MESSAGE>],[<LOCAL_IP>][,<REMOTE_IP>]
 */
void
OnStateChange(connection_t *c, char *data)
{
    char *pos, *state, *message;

    pos = strchr(data, ',');
    if (pos == NULL)
        return;
    *pos = '\0';

    state = pos + 1;
    pos = strchr(state, ',');
    if (pos == NULL)
        return;
    *pos = '\0';

    message = pos + 1;
    pos = strchr(message, ',');
    if (pos == NULL)
        return;
    *pos = '\0';

    if (strcmp(state, "CONNECTED") == 0)
    {
        /* Run Connect Script */
        if (c->state == connecting || c->state == resuming)
            RunConnectScript(c, false);

        /* Save the local IP address if available */
        char *local_ip = pos + 1;
        pos = strchr(local_ip, ',');
        if (pos != NULL)
            *pos = '\0';

        /* Convert the IP address to Unicode */
        MultiByteToWideChar(CP_ACP, 0, local_ip, -1, c->ip, _countof(c->ip));

        /* Show connection tray balloon */
        if ((c->state == connecting   && o.show_balloon != 0)
        ||  (c->state == resuming     && o.show_balloon != 0)
        ||  (c->state == reconnecting && o.show_balloon == 2))
        {
            TCHAR msg[256];
            LoadLocalizedStringBuf(msg, _countof(msg), IDS_NFO_NOW_CONNECTED, c->config_name);
            ShowTrayBalloon(msg, (_tcslen(c->ip) ? LoadLocalizedString(IDS_NFO_ASSIGN_IP, c->ip) : _T("")));
        }

        /* Save time when we got connected. */
        c->connected_since = atoi(data);
        c->failed_psw_attempts = 0;
        c->state = connected;

        SetMenuStatus(c, connected);
        SetTrayIcon(connected);

        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_CONNECTED));
        SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTED);

        /* Hide Status Window */
        ShowWindow(c->hwndStatus, SW_HIDE);
    }
    else if (strcmp(state, "RECONNECTING") == 0)
    {
        if (strcmp(message, "auth-failure") == 0
        ||  strcmp(message, "private-key-password-failure") == 0)
            c->failed_psw_attempts++;

        if (strcmp(message, "auth-failure") == 0 && (c->flags & FLAG_SAVE_AUTH_PASS))
            SaveAuthPass(c->config_name, L"");
        else if (strcmp(message, "private-key-password-failure") == 0 && (c->flags & FLAG_SAVE_KEY_PASS))
            SaveKeyPass(c->config_name, L"");

        c->state = reconnecting;
        CheckAndSetTrayIcon();

        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_RECONNECTING));
        SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTING);
    }
}


/*
 * DialogProc for OpenVPN username/password/challenge auth dialog windows
 */
INT_PTR CALLBACK
UserAuthDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auth_param_t *param;
    WCHAR username[USER_PASS_LEN];
    WCHAR password[USER_PASS_LEN];

    switch (msg)
    {
    case WM_INITDIALOG:
        /* Set connection for this dialog and show it */
        param = (auth_param_t *) lParam;
        SetProp(hwndDlg, cfgProp, (HANDLE) param);
        if (param->challenge_str)
        {
            int wchars_num = MultiByteToWideChar(CP_UTF8, 0, param->challenge_str, -1, NULL, 0);
            LPWSTR wstr = (LPWSTR)malloc(sizeof(WCHAR) * wchars_num);
            HWND wnd_challenge = GetDlgItem(hwndDlg, ID_EDT_AUTH_CHALLENGE);

            MultiByteToWideChar(CP_UTF8, 0, param->challenge_str, -1, wstr, wchars_num);
            SetDlgItemTextW(hwndDlg, ID_TXT_AUTH_CHALLENGE, wstr);
            free(wstr);
            /* Set/Remove style ES_PASSWORD by SetWindowLong(GWL_STYLE) does nothing,
               send EM_SETPASSWORDCHAR just works. */
            if(param->challenge_echo)
                SendMessage(wnd_challenge, EM_SETPASSWORDCHAR, 0, 0);
        }
        if (RecallUsername(param->c->config_name, username))
            SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_USER, username);
        if (RecallAuthPass(param->c->config_name, password))
        {
            SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_PASS, password);
            SecureZeroMemory(password, sizeof(password));
        }
        if (param->c->flags & FLAG_SAVE_AUTH_PASS)
            Button_SetCheck(GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);

        if (param->c->state == resuming)
            ForceForegroundWindow(hwndDlg);
        else
            SetForegroundWindow(hwndDlg);
        break;

    case WM_COMMAND:
        param = (auth_param_t *) GetProp(hwndDlg, cfgProp);
        switch (LOWORD(wParam))
        {
        case ID_EDT_AUTH_USER:
            if (HIWORD(wParam) == EN_UPDATE)
            {
                int len = Edit_GetTextLength((HWND) lParam);
                EnableWindow(GetDlgItem(hwndDlg, IDOK), (len ? TRUE : FALSE));
            }
            break;

        case ID_CHK_SAVE_PASS:
            param->c->flags ^= FLAG_SAVE_AUTH_PASS;
            if (param->c->flags & FLAG_SAVE_AUTH_PASS)
                Button_SetCheck(GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
            else
            {
                DeleteSavedAuthPass(param->c->config_name);
                Button_SetCheck(GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_UNCHECKED);
            }
            break;

        case IDOK:
            if (GetDlgItemTextW(hwndDlg, ID_EDT_AUTH_USER, username, _countof(username)))
            {
                SaveUsername(param->c->config_name, username);
            }
            if ( param->c->flags & FLAG_SAVE_AUTH_PASS &&
                 GetDlgItemTextW(hwndDlg, ID_EDT_AUTH_PASS, password, _countof(password)) &&
                 wcslen(password) )
            {
                SaveAuthPass(param->c->config_name, password);
                SecureZeroMemory(password, sizeof(password));
            }
            ManagementCommandFromInput(param->c, "username \"Auth\" \"%s\"", hwndDlg, ID_EDT_AUTH_USER);
            if (param->challenge_str)
                ManagementCommandFromInputBase64(param->c, "password \"Auth\" \"SCRV1:%s:%s\"", hwndDlg, ID_EDT_AUTH_PASS, ID_EDT_AUTH_CHALLENGE);
            else
                ManagementCommandFromInput(param->c, "password \"Auth\" \"%s\"", hwndDlg, ID_EDT_AUTH_PASS);
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case IDCANCEL:
            EndDialog(hwndDlg, LOWORD(wParam));
            StopOpenVPN(param->c);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwndDlg, LOWORD(wParam));
        return TRUE;

    case WM_NCDESTROY:
        param = (auth_param_t *) GetProp(hwndDlg, cfgProp);
        if (param->challenge_str) free(param->challenge_str);
        free(param);
        RemoveProp(hwndDlg, cfgProp);
        break;
    }

    return FALSE;
}


/*
 * DialogProc for OpenVPN private key password dialog windows
 */
INT_PTR CALLBACK
PrivKeyPassDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    connection_t *c = NULL;
    WCHAR passphrase[KEY_PASS_LEN];

    auth_param_t *param = (auth_param_t *) lParam;
    char cmd[200];
    const char *cmd_private_key = "password \"Private Key\" \"%%s\"";
    const char *cmd_other_key   = "password \"%s\" \"%%s\"";

    switch (msg)
    {
    case WM_INITDIALOG:
        /* Set connection for this dialog and show it */
        SetProp(hwndDlg, cfgProp, (HANDLE) param);
        c = param->c;
        if (RecallKeyPass(c->config_name, passphrase) && wcslen(passphrase))
        {
            /* Use the saved password and skip the dialog */
            SetDlgItemTextW(hwndDlg, ID_EDT_PASSPHRASE, passphrase);
            SecureZeroMemory(passphrase, sizeof(passphrase));
            if (param->challenge_echo) {
                snprintf(cmd, sizeof(cmd), cmd_other_key, param->challenge_str);
            } else {
                snprintf(cmd, sizeof(cmd), cmd_private_key);
            }
            ManagementCommandFromInput(c, cmd, hwndDlg, ID_EDT_PASSPHRASE);
            EndDialog(hwndDlg, IDOK);
            return TRUE;
        }
        if (c->flags & FLAG_SAVE_KEY_PASS)
            Button_SetCheck (GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
        if (c->state == resuming)
            ForceForegroundWindow(hwndDlg);
        else
            SetForegroundWindow(hwndDlg);
        break;

    case WM_COMMAND:
        param = (auth_param_t *) GetProp(hwndDlg, cfgProp);
        c = param->c;
        switch (LOWORD(wParam))
        {
        case ID_CHK_SAVE_PASS:
            c->flags ^= FLAG_SAVE_KEY_PASS;
            if (c->flags & FLAG_SAVE_KEY_PASS)
                Button_SetCheck (GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
            else
            {
                Button_SetCheck (GetDlgItem (hwndDlg, ID_CHK_SAVE_PASS), BST_UNCHECKED);
                DeleteSavedKeyPass(c->config_name);
            }
            break;

        case IDOK:
            if ((c->flags & FLAG_SAVE_KEY_PASS) &&
                GetDlgItemTextW(hwndDlg, ID_EDT_PASSPHRASE, passphrase, _countof(passphrase)) &&
                wcslen(passphrase) > 0)
            {
                SaveKeyPass(c->config_name, passphrase);
                SecureZeroMemory(passphrase, sizeof(passphrase));
            }
            if (param->challenge_echo) {
                snprintf(cmd, sizeof(cmd), cmd_other_key, param->challenge_str);
            } else {
                snprintf(cmd, sizeof(cmd), cmd_private_key);
            }
            ManagementCommandFromInput(c, cmd, hwndDlg, ID_EDT_PASSPHRASE);
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case IDCANCEL:
            EndDialog(hwndDlg, LOWORD(wParam));
            StopOpenVPN (c);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwndDlg, LOWORD(wParam));
        return TRUE;

    case WM_NCDESTROY:
        RemoveProp(hwndDlg, cfgProp);
        break;
  }
  return FALSE;
}


/*
 * Handle the request to release a hold from the OpenVPN management interface
 */
void
OnPassword(connection_t *c, char *msg)
{
    if (strncmp(msg, "Verification Failed", 19) == 0)
        return;

    if (strstr(msg, "'Auth'"))
    {
        char* chstr = strstr(msg, "SC:");
        auth_param_t *param = (auth_param_t *) malloc(sizeof(auth_param_t));
        param->c = c;
        if (chstr)
        {
            param->challenge_echo = *(chstr + 3) != '0';
            param->challenge_str = strdup(chstr + 5);
            LocalizedDialogBoxParam(ID_DLG_AUTH_CHALLENGE, UserAuthDialogFunc, (LPARAM) param);
        }
        else
        {
            param->challenge_echo = 0;
            param->challenge_str = NULL;
            LocalizedDialogBoxParam(ID_DLG_AUTH, UserAuthDialogFunc, (LPARAM) param);
        }
    }
    else if (strstr(msg, "'Private Key'"))
    {
        auth_param_t *param = (auth_param_t *) malloc(sizeof(auth_param_t));
        param->challenge_echo = 0;
        param->challenge_str = "";
        param->c = c;
        LocalizedDialogBoxParam(ID_DLG_PASSPHRASE, PrivKeyPassDialogFunc, (LPARAM) param);
    }
    else if (strstr(msg, "'HTTP Proxy'"))
    {
        QueryProxyAuth(c, http);
    }
    else if (strstr(msg, "'SOCKS Proxy'"))
    {
        QueryProxyAuth(c, socks);
    }
    else
    {
        /* process all other auth requests, e.g. PIN queries from smartcards */
        /* fix for https://community.openvpn.net/openvpn/ticket/740 */
        char *msg2 = strdup(msg);
        char *sep = msg2;
        char *saveptr;
        auth_param_t *param = (auth_param_t *) malloc(sizeof (auth_param_t));
        param->challenge_echo = 1;
        param->challenge_str = "";
        param->c = c;
        sep = strtok_r(sep, "'", &saveptr);
        if (sep != NULL) {
            sep = strtok_r(NULL, "'", &saveptr);
            /* extract the auth name, e.g. "PIV_II (PIV Card Holder pin) token"
             * from ">PASSWORD:Need 'PIV_II (PIV Card Holder pin) token' password" */
            param->challenge_str = strdup(sep);
        }
        /* TODO: Make an extra dialog
         * The PrivKeyPassDialogFunc dialog does not match all requirements:
         *
         * 1) users normally must not save PINs, e.g. for security reasons and
         *    because false PIN inputs may lock the PINs after some attempts
         * 2) it may be possible to detect PIN requests, e.g. by the word "pin"
         * 3) the auth name is not displayed in dialog
         * 4) the data structure auth_param_t does not match here
         */
        LocalizedDialogBoxParam(ID_DLG_PASSPHRASE, PrivKeyPassDialogFunc, (LPARAM) param);
        free(msg2);
    }
}


/*
 * Handle exit of the OpenVPN process
 */
void
OnStop(connection_t *c, UNUSED char *msg)
{
    UINT txt_id, msg_id;
    TCHAR *msg_xtra;
    SetMenuStatus(c, disconnected);

    switch (c->state)
    {
    case connected:
        /* OpenVPN process ended unexpectedly */
        c->failed_psw_attempts = 0;
        c->state = disconnected;
        CheckAndSetTrayIcon();
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_DISCONNECTED));
        SetStatusWinIcon(c->hwndStatus, ID_ICO_DISCONNECTED);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
        if (o.silent_connection == 0)
        {
            SetForegroundWindow(c->hwndStatus);
            ShowWindow(c->hwndStatus, SW_SHOW);
        }
        ShowLocalizedMsg(IDS_NFO_CONN_TERMINATED, c->config_name);
        SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
        break;

    case resuming:
    case connecting:
    case reconnecting:
    case timedout:
        /* We have failed to (re)connect */
        txt_id = c->state == reconnecting ? IDS_NFO_STATE_FAILED_RECONN : IDS_NFO_STATE_FAILED;
        msg_id = c->state == reconnecting ? IDS_NFO_RECONN_FAILED : IDS_NFO_CONN_FAILED;
        msg_xtra = c->state == timedout ? c->log_path : c->config_name;
        if (c->state == timedout)
            msg_id = IDS_NFO_CONN_TIMEOUT;

        c->state = disconnecting;
        CheckAndSetTrayIcon();
        c->state = disconnected;
        EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
        SetStatusWinIcon(c->hwndStatus, ID_ICO_DISCONNECTED);
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(txt_id));
        if (o.silent_connection == 0)
        {
            SetForegroundWindow(c->hwndStatus);
            ShowWindow(c->hwndStatus, SW_SHOW);
        }
        ShowLocalizedMsg(msg_id, msg_xtra);
        SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
        break;

    case disconnecting:
//   /* Check for "certificate has expired" message */
//   if ((strstr(line, "error=certificate has expired") != NULL))
//     {
//       StopOpenVPN(config);
//       /* Cert expired... */
//       ShowLocalizedMsg(IDS_ERR_CERT_EXPIRED);
//     }
//
//   /* Check for "certificate is not yet valid" message */
//   if ((strstr(line, "error=certificate is not yet valid") != NULL))
//     {
//       StopOpenVPN(config);
//       /* Cert not yet valid */
//       ShowLocalizedMsg(IDS_ERR_CERT_NOT_YET_VALID);
//     }
        /* Shutdown was initiated by us */
        c->failed_psw_attempts = 0;
        c->state = disconnected;
        CheckAndSetTrayIcon();
        SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
        break;

    case suspending:
        c->state = suspended;
        CheckAndSetTrayIcon();
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_SUSPENDED));
        break;

    default:
        break;
    }
}

/*
 * Break a long line into shorter segments
 */
static WCHAR *
WrapLine (WCHAR *line)
{
    int i = 0;
    WCHAR *next = NULL;
    int len = 80;

    for (i = 0; *line; i++, ++line)
    {
        if ((*line == L'\r') || (*line == L'\n'))
            *line = L' ';
        if (next && i > len) break;
        if (iswspace(*line))  next = line;
    }
    if (!*line) next = NULL;
    if (next)
    {
        *next = L'\0';
        ++next;
    }
    return next;
}

/*
 * Write a line to the status log window and optionally to the log file
 */
static void
WriteStatusLog (connection_t *c, const WCHAR *prefix, const WCHAR *line, BOOL fileio)
{
    HWND logWnd = GetDlgItem(c->hwndStatus, ID_EDT_LOG);
    FILE *log_fd;
    time_t now;
    WCHAR datetime[26];

    time (&now);
    /* TODO: change this to use _wctime_s when mingw supports it */
    wcsncpy (datetime, _wctime(&now), _countof(datetime));
    datetime[24] = L' ';

    /* Remove lines from log window if it is getting full */
    if (SendMessage(logWnd, EM_GETLINECOUNT, 0, 0) > MAX_LOG_LINES)
    {
        int pos = SendMessage(logWnd, EM_LINEINDEX, DEL_LOG_LINES, 0);
        SendMessage(logWnd, EM_SETSEL, 0, pos);
        SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T(""));
    }
    /* Append line to log window */
    SendMessage(logWnd, EM_SETSEL, (WPARAM) -1, (LPARAM) -1);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) datetime);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) prefix);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) line);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) L"\n");

    if (!fileio) return;

    log_fd = _tfopen (c->log_path, TEXT("at+,ccs=UTF-8"));
    if (log_fd)
    {
        fwprintf (log_fd, L"%s%s%s\n", datetime, prefix, line);
        fclose (log_fd);
    }
}

#define IO_TIMEOUT 5000 /* milliseconds */

static void
CloseServiceIO (service_io_t *s)
{
    if (s->hEvent)
        CloseHandle(s->hEvent);
    s->hEvent = NULL;
    if (s->pipe && s->pipe != INVALID_HANDLE_VALUE)
        CloseHandle(s->pipe);
    s->pipe = NULL;
}

/*
 * Open the service pipe and initialize service I/O.
 * Failure is not fatal.
 */
static BOOL
InitServiceIO (service_io_t *s)
{
    DWORD dwMode = PIPE_READMODE_MESSAGE;
    CLEAR(*s);

    /* auto-reset event used for signalling i/o completion*/
    s->hEvent = CreateEvent (NULL, FALSE, FALSE, NULL);
    if (!s->hEvent)
    {
        return FALSE;
    }

    s->pipe = CreateFile(_T("\\\\.\\pipe\\openvpn\\service"),
                GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if ( !s->pipe                                               ||
         s->pipe == INVALID_HANDLE_VALUE                        ||
         !SetNamedPipeHandleState(s->pipe, &dwMode, NULL, NULL)
       )
    {
        CloseServiceIO (s);
        return FALSE;
    }

    return TRUE;
}

/*
 * Read-completion routine for interactive service pipe. Call with
 * err = 0, bytes = 0 to queue the first read request.
 */
static void
HandleServiceIO (DWORD err, DWORD bytes, LPOVERLAPPED lpo)
{
    service_io_t *s = (service_io_t *) lpo;
    int len, capacity;

    len = _countof(s->readbuf);
    capacity = len*sizeof(*(s->readbuf));

    if (bytes > 0)
        SetEvent (s->hEvent);
    if (err)
    {
        _snwprintf(s->readbuf, len, L"0x%08x\nInteractive Service disconnected\n", err);
        s->readbuf[len-1] = L'\0';
        SetEvent (s->hEvent);
        return;
    }

    /* queue next read request */
    ReadFileEx (s->pipe, s->readbuf, capacity, lpo, (LPOVERLAPPED_COMPLETION_ROUTINE) HandleServiceIO);
    /* Any error in the above call will get checked in next round */
}

/*
 * Write size bytes in buf to the pipe with a timeout.
 * Retun value: TRUE on success FLASE on error
 */
static BOOL
WritePipe (HANDLE pipe, LPVOID buf, DWORD size)
{
    OVERLAPPED o;
    BOOL retval = FALSE;

    CLEAR(o);
    o.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);

    if (!o.hEvent)
    {
        return retval;
    }

    if (WriteFile (pipe, buf, size, NULL, &o)  ||
        GetLastError() == ERROR_IO_PENDING )
    {
        if (WaitForSingleObject(o.hEvent, IO_TIMEOUT) == WAIT_OBJECT_0)
            retval = TRUE;
        else
            CancelIo (pipe);
            // TODO report error -- timeout
    }

    CloseHandle(o.hEvent);
    return retval;
}

/*
 * Called when read from service pipe signals
 */
static void
OnService(connection_t *c, UNUSED char *msg)
{
    DWORD err = 0;
    WCHAR *p, *buf, *next;
    DWORD len;
    const WCHAR *prefix = L"IService> ";

    len = wcslen (c->iserv.readbuf);
    if (!len || (buf = wcsdup (c->iserv.readbuf)) == NULL)
        return;

    /* messages from the service are in the format "0x08x\n%s\n%s" */
    if (swscanf (buf, L"0x%08x\n", &err) != 1)
    {
        free (buf);
        return;
    }

    p = buf + 11;
    while (iswspace(*p)) ++p;

    while (p && *p)
    {
        next = WrapLine (p);
        WriteStatusLog (c, prefix, p, c->manage.connected ? FALSE : TRUE);
        p = next;
    }
    free (buf);

    /* Error from iservice before management interface is connected */
    switch (err)
    {
        case 0:
            break;
        case ERROR_STARTUP_DATA:
            WriteStatusLog (c, prefix, L"OpenVPN not started due to previous errors", true);
            c->state = timedout;   /* Force the popup message to include the log file name */
            OnStop (c, NULL);
            break;
        case ERROR_OPENVPN_STARTUP:
            WriteStatusLog (c, prefix, L"Check the log file for details", false);
            c->state = timedout;   /* Force the popup message to include the log file name */
            OnStop(c, NULL);
            break;
        default:
            /* Unknown failure: let management connection timeout */
            break;
    }
}

/*
 * Called when the directly started openvpn process exits
 */
static void
OnProcess (connection_t *c, UNUSED char *msg)
{
    DWORD err;
    WCHAR tmp[256];

    if (!GetExitCodeProcess(c->hProcess, &err) || err == STILL_ACTIVE)
        return;

    _snwprintf(tmp, _countof(tmp),  L"OpenVPN terminated with exit code %lu. "
                                    L"See the log file for details", err);
    tmp[_countof(tmp)-1] = L'\0';
    WriteStatusLog(c, L"OpenVPN GUI> ", tmp, false);

    OnStop (c, NULL);
}

/*
 * Close open handles
 */
static void
Cleanup (connection_t *c)
{
    CloseManagement (c);

    if (c->hProcess)
        CloseHandle (c->hProcess);
    else
        CloseServiceIO (&c->iserv);
    c->hProcess = NULL;

    if (c->exit_event)
        CloseHandle (c->exit_event);
    c->exit_event = NULL;
}

/*
 * DialogProc for OpenVPN status dialog windows
 */
INT_PTR CALLBACK
StatusDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    connection_t *c;

    switch (msg)
    {
    case WM_MANAGEMENT:
        /* Management interface related event */
        OnManagement(wParam, lParam);
        return TRUE;

    case WM_INITDIALOG:
        c = (connection_t *) lParam;

        /* Set window icon "disconnected" */
        SetStatusWinIcon(hwndDlg, ID_ICO_CONNECTING);

        /* Set connection for this dialog */
        SetProp(hwndDlg, cfgProp, (HANDLE) c);

        /* Create log window */
        HWND hLogWnd = CreateWindowEx(0, RICHEDIT_CLASS, NULL,
            WS_CHILD|WS_VISIBLE|WS_HSCROLL|WS_VSCROLL|ES_SUNKEN|ES_LEFT|
            ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL|ES_AUTOVSCROLL,
            20, 25, 350, 160, hwndDlg, (HMENU) ID_EDT_LOG, o.hInstance, NULL);
        if (!hLogWnd)
        {
            ShowLocalizedMsg(IDS_ERR_CREATE_EDIT_LOGWINDOW);
            return FALSE;
        }

        /* Set font and fontsize of the log window */
        CHARFORMAT cfm = {
            .cbSize = sizeof(CHARFORMAT),
            .dwMask = CFM_SIZE|CFM_FACE|CFM_BOLD,
            .szFaceName = _T("Microsoft Sans Serif"),
            .dwEffects = 0,
            .yHeight = 160
        };
        if (SendMessage(hLogWnd, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM) &cfm) == 0)
            ShowLocalizedMsg(IDS_ERR_SET_SIZE);

        /* Set size and position of controls */
        RECT rect;
        GetClientRect(hwndDlg, &rect);
        MoveWindow(hLogWnd, 20, 25, rect.right - 40, rect.bottom - 70, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_TXT_STATUS), 20, 5, rect.right - 25, 15, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_DISCONNECT), 20, rect.bottom - 30, 110, 23, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_RESTART), 145, rect.bottom - 30, 110, 23, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_HIDE), rect.right - 130, rect.bottom - 30, 110, 23, TRUE);

        /* Set focus on the LogWindow so it scrolls automatically */
        SetFocus(hLogWnd);
        return FALSE;

    case WM_SIZE:
        MoveWindow(GetDlgItem(hwndDlg, ID_EDT_LOG), 20, 25, LOWORD(lParam) - 40, HIWORD(lParam) - 70, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_DISCONNECT), 20, HIWORD(lParam) - 30, 110, 23, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_RESTART), 145, HIWORD(lParam) - 30, 110, 23, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_HIDE), LOWORD(lParam) - 130, HIWORD(lParam) - 30, 110, 23, TRUE);
        MoveWindow(GetDlgItem(hwndDlg, ID_TXT_STATUS), 20, 5, LOWORD(lParam) - 25, 15, TRUE);
        InvalidateRect(hwndDlg, NULL, TRUE);
        return TRUE;

    case WM_COMMAND:
        c = (connection_t *) GetProp(hwndDlg, cfgProp);
        switch (LOWORD(wParam))
        {
        case ID_DISCONNECT:
            SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
            StopOpenVPN(c);
            return TRUE;

        case ID_HIDE:
            if (c->state != disconnected)
                ShowWindow(hwndDlg, SW_HIDE);
            else
                DestroyWindow(hwndDlg);
            return TRUE;

        case ID_RESTART:
            c->state = reconnecting;
            SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
            ManagementCommand(c, "signal SIGHUP", NULL, regular);
            return TRUE;
        }
        break;

    case WM_SHOWWINDOW:
        if (wParam == TRUE)
        {
            c = (connection_t *) GetProp(hwndDlg, cfgProp);
            if (c->hwndStatus)
                SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
        }
        return FALSE;

    case WM_CLOSE:
        c = (connection_t *) GetProp(hwndDlg, cfgProp);
        if (c->state != disconnected)
            ShowWindow(hwndDlg, SW_HIDE);
        else
            DestroyWindow(hwndDlg);
        return TRUE;

    case WM_NCDESTROY:
        RemoveProp(hwndDlg, cfgProp);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_OVPN_STOP:
        c = (connection_t *) GetProp(hwndDlg, cfgProp);
        c->state = disconnecting;
        RunDisconnectScript(c, false);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
        SetMenuStatus(c, disconnecting);
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_WAIT_TERM));
        SetEvent(c->exit_event);
        break;

    case WM_OVPN_SUSPEND:
        c = (connection_t *) GetProp(hwndDlg, cfgProp);
        c->state = suspending;
        EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
        SetMenuStatus(c, disconnecting);
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_WAIT_TERM));
        SetEvent(c->exit_event);
        break;
    }
    return FALSE;
}

/*
 * ThreadProc for OpenVPN status dialog windows
 */
static DWORD WINAPI
ThreadOpenVPNStatus(void *p)
{
    connection_t *c = p;
    TCHAR conn_name[200];
    MSG msg;
    HANDLE wait_event;

    CLEAR (msg);

    /* Cut of extention from config filename. */
    _tcsncpy(conn_name, c->config_file, _countof(conn_name));
    conn_name[_tcslen(conn_name) - _tcslen(o.ext_string) - 1] = _T('\0');

    c->state = (c->state == suspended ? resuming : connecting);

    /* Create and Show Status Dialog */
    c->hwndStatus = CreateLocalizedDialogParam(ID_DLG_STATUS, StatusDialogFunc, (LPARAM) c);
    if (!c->hwndStatus)
        return 1;

    CheckAndSetTrayIcon();
    SetMenuStatus(c, connecting);
    SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_CONNECTING));
    SetWindowText(c->hwndStatus, LoadLocalizedString(IDS_NFO_CONNECTION_XXX, conn_name));

    if (!OpenManagement(c))
        PostMessage(c->hwndStatus, WM_CLOSE, 0, 0);

    /* Start the async read loop for service and set it as the wait event */
    if (!c->hProcess)
    {
        HandleServiceIO (0, 0, (LPOVERLAPPED) &c->iserv);
        wait_event = c->iserv.hEvent;
    }
    else
        wait_event = c->hProcess;

    if (o.silent_connection == 0)
        ShowWindow(c->hwndStatus, SW_SHOW);

    /* Run the message loop for the status window */
    while (WM_QUIT != msg.message)
    {
        DWORD res;
        if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if ((res = MsgWaitForMultipleObjectsEx (1, &wait_event, INFINITE, QS_ALLINPUT,
                                         MWMO_ALERTABLE)) == WAIT_OBJECT_0)
            {
                if (c->hProcess)
                    OnProcess (c, NULL);
                else
                    OnService (c, NULL);
            }
            continue;
        }

        if (IsDialogMessage(c->hwndStatus, &msg) == 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    /* release handles etc.*/
    Cleanup (c);
    return 0;
}

/*
 * Set priority based on the registry or cmd-line value
 */
static BOOL
SetProcessPriority(DWORD *priority)
{
    *priority = NORMAL_PRIORITY_CLASS;
    if (!_tcscmp(o.priority_string, _T("IDLE_PRIORITY_CLASS")))
        *priority = IDLE_PRIORITY_CLASS;
    else if (!_tcscmp(o.priority_string, _T("BELOW_NORMAL_PRIORITY_CLASS")))
        *priority = BELOW_NORMAL_PRIORITY_CLASS;
    else if (!_tcscmp(o.priority_string, _T("NORMAL_PRIORITY_CLASS")))
        *priority = NORMAL_PRIORITY_CLASS;
    else if (!_tcscmp(o.priority_string, _T("ABOVE_NORMAL_PRIORITY_CLASS")))
        *priority = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (!_tcscmp(o.priority_string, _T("HIGH_PRIORITY_CLASS")))
        *priority = HIGH_PRIORITY_CLASS;
    else
    {
        ShowLocalizedMsg(IDS_ERR_UNKNOWN_PRIORITY, o.priority_string);
        return FALSE;
    }
    return TRUE;
}

/*
 * Launch an OpenVPN process and the accompanying thread to monitor it
 */
BOOL
StartOpenVPN(connection_t *c)
{
    TCHAR cmdline[1024];
    TCHAR *options = cmdline + 8;
    TCHAR exit_event_name[17];
    HANDLE hStdInRead = NULL, hStdInWrite = NULL;
    HANDLE hNul = NULL, hThread = NULL;
    DWORD written;
    BOOL retval = FALSE;

    CLEAR(c->ip);

    RunPreconnectScript(c);

    /* Create thread to show the connection's status dialog */
    hThread = CreateThread(NULL, 0, ThreadOpenVPNStatus, c, CREATE_SUSPENDED, &c->threadId);
    if (hThread == NULL)
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_THREAD_STATUS);
        goto out;
    }

    /* Create an event object to signal OpenVPN to exit */
    _sntprintf_0(exit_event_name, _T("%x%08x"), GetCurrentProcessId(), c->threadId);
    c->exit_event = CreateEvent(NULL, TRUE, FALSE, exit_event_name);
    if (c->exit_event == NULL)
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_EVENT, exit_event_name);
        goto out;
    }

    /* Create a management interface password */
    GetRandomPassword(c->manage.password, sizeof(c->manage.password) - 1);

    /* Construct command line -- put log first */
    _sntprintf_0(cmdline, _T("openvpn --log%s \"%s\" --config \"%s\" "
        "--setenv IV_GUI_VER \"%S\" --service %s 0 --auth-retry interact "
        "--management %S %hd stdin --management-query-passwords %s"
        "--management-hold"),
        (o.log_append ? _T("-append") : _T("")), c->log_path,
        c->config_file, PACKAGE_STRING, exit_event_name,
        inet_ntoa(c->manage.skaddr.sin_addr), ntohs(c->manage.skaddr.sin_port),
        (o.proxy_source != config ? _T("--management-query-proxy ") : _T("")));

    /* Try to open the service pipe */
    if (!IsUserAdmin() && InitServiceIO (&c->iserv))
    {
        DWORD size = _tcslen(c->config_dir) + _tcslen(options) + sizeof(c->manage.password) + 3;
        TCHAR startup_info[1024];

        if ( !AuthorizeConfig(c))
        {
            CloseHandle(c->exit_event);
            goto out;
        }

        c->hProcess = NULL;
        c->manage.password[sizeof(c->manage.password) - 1] = '\n';
        _sntprintf_0(startup_info, _T("%s%c%s%c%.*S"), c->config_dir, _T('\0'),
            options, _T('\0'), sizeof(c->manage.password), c->manage.password);
        c->manage.password[sizeof(c->manage.password) - 1] = '\0';

        if (!WritePipe(c->iserv.pipe, startup_info, size * sizeof (TCHAR)))
        {
            ShowLocalizedMsg (IDS_ERR_WRITE_SERVICE_PIPE);
            CloseHandle(c->exit_event);
            CloseServiceIO(&c->iserv);
            goto out;
        }
    }
    else
    {
        /* Start OpenVPN directly */
        DWORD priority;
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        SECURITY_DESCRIPTOR sd;

        /* Make I/O handles inheritable and accessible by all */
        SECURITY_ATTRIBUTES sa = {
            .nLength = sizeof(sa),
            .lpSecurityDescriptor = &sd,
            .bInheritHandle = TRUE
        };

        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
        {
            ShowLocalizedMsg(IDS_ERR_INIT_SEC_DESC);
            CloseHandle(c->exit_event);
            return FALSE;
        }
        if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
        {
            ShowLocalizedMsg(IDS_ERR_SET_SEC_DESC_ACL);
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Set process priority */
        if (!SetProcessPriority(&priority))
        {
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Get a handle of the NUL device */
        hNul = CreateFile(_T("NUL"), GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (hNul == INVALID_HANDLE_VALUE)
        {
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Create the pipe for STDIN with only the read end inheritable */
        if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0))
        {
            ShowLocalizedMsg(IDS_ERR_CREATE_PIPE_IN_READ);
            CloseHandle(c->exit_event);
            goto out;
        }
        if (!SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0))
        {
            ShowLocalizedMsg(IDS_ERR_DUP_HANDLE_IN_WRITE);
            CloseHandle(c->exit_event);
            goto out;
        }

        /* Fill in STARTUPINFO struct */
        GetStartupInfo(&si);
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hStdInRead;
        si.hStdOutput = hNul;
        si.hStdError = hNul;

        /* Create an OpenVPN process for the connection */
        if (!CreateProcess(o.exe_path, cmdline, NULL, NULL, TRUE,
                        priority | CREATE_NO_WINDOW, NULL, c->config_dir, &si, &pi))
        {
            ShowLocalizedMsg(IDS_ERR_CREATE_PROCESS, o.exe_path, cmdline, c->config_dir);
            CloseHandle(c->exit_event);
            goto out;
        }

        /* Pass management password to OpenVPN process */
        c->manage.password[sizeof(c->manage.password) - 1] = '\n';
        WriteFile(hStdInWrite, c->manage.password, sizeof(c->manage.password), &written, NULL);
        c->manage.password[sizeof(c->manage.password) - 1] = '\0';

        c->hProcess = pi.hProcess; /* Will be closed in the event loop on exit */
        CloseHandle(pi.hThread);
    }

    /* Start the status dialog thread */
    ResumeThread(hThread);
    retval = TRUE;

out:
    if (hThread && hThread != INVALID_HANDLE_VALUE)
        CloseHandle(hThread);
    if (hStdInWrite && hStdInWrite != INVALID_HANDLE_VALUE)
        CloseHandle(hStdInWrite);
    if (hStdInRead && hStdInRead != INVALID_HANDLE_VALUE)
        CloseHandle(hStdInRead);
    if (hNul && hNul != INVALID_HANDLE_VALUE)
        CloseHandle(hNul);
    return retval;
}


void
StopOpenVPN(connection_t *c)
{
    PostMessage(c->hwndStatus, WM_OVPN_STOP, 0, 0);
}


void
SuspendOpenVPN(int config)
{
    PostMessage(o.conn[config].hwndStatus, WM_OVPN_SUSPEND, 0, 0);
}


void
SetStatusWinIcon(HWND hwndDlg, int iconId)
{
    HICON hIcon = LoadLocalizedIcon(iconId);
    if (!hIcon)
        return;

    SendMessage(hwndDlg, WM_SETICON, (WPARAM) ICON_SMALL, (LPARAM) hIcon);
    SendMessage(hwndDlg, WM_SETICON, (WPARAM) ICON_BIG, (LPARAM) hIcon);
}


/*
 * Read one line from OpenVPN's stdout.
 */
static BOOL
ReadLineFromStdOut(HANDLE hStdOut, char *line, DWORD size)
{
    DWORD len, read;

    while (TRUE)
    {
        if (!PeekNamedPipe(hStdOut, line, size, &read, NULL, NULL))
        {
            if (GetLastError() != ERROR_BROKEN_PIPE)
                ShowLocalizedMsg(IDS_ERR_READ_STDOUT_PIPE);
            return FALSE;
        }

        char *pos = memchr(line, '\r', read);
        if (pos)
        {
            len = pos - line + 2;
            if (len > size)
                return FALSE;
            break;
        }

        /* Line doesn't fit into the buffer */
        if (read == size)
            return FALSE;

        Sleep(100);
    }

    if (!ReadFile(hStdOut, line, len, &read, NULL) || read != len)
    {
        if (GetLastError() != ERROR_BROKEN_PIPE)
            ShowLocalizedMsg(IDS_ERR_READ_STDOUT_PIPE);
        return FALSE;
    }

    line[read - 2] = '\0';
    return TRUE;
}


BOOL
CheckVersion()
{
    HANDLE hStdOutRead;
    HANDLE hStdOutWrite;
    BOOL retval = FALSE;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    TCHAR cmdline[] = _T("openvpn --version");
    char match_version[] = "OpenVPN 2.";
    TCHAR pwd[MAX_PATH];
    char line[1024];
    TCHAR *p;

    CLEAR(si);
    CLEAR(pi);

    /* Make handles inheritable and accessible by all */
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa = {
        .nLength = sizeof(sa),
        .lpSecurityDescriptor = &sd,
        .bInheritHandle = TRUE
    };
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
    {
        ShowLocalizedMsg(IDS_ERR_INIT_SEC_DESC);
        return FALSE;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
    {
        ShowLocalizedMsg(IDS_ERR_SET_SEC_DESC_ACL);
        return FALSE;
    }

    /* Create the pipe for STDOUT with inheritable write end */
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0))
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_PIPE_IN_READ);
        return FALSE;
    }
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0))
    {
        ShowLocalizedMsg(IDS_ERR_DUP_HANDLE_IN_WRITE);
        goto out;
    }

    /* Construct the process' working directory */
    _tcsncpy(pwd, o.exe_path, _countof(pwd));
    p = _tcsrchr(pwd, _T('\\'));
    if (p != NULL)
        *p = _T('\0');

    /* Fill in STARTUPINFO struct */
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    /* Start OpenVPN to check version */
    if (!CreateProcess(o.exe_path, cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, pwd, &si, &pi))
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_PROCESS, o.exe_path, cmdline, pwd);
    }
    else if (ReadLineFromStdOut(hStdOutRead, line, sizeof(line)))
    {
#ifdef DEBUG
        PrintDebug(_T("VersionString: %S"), line);
#endif
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        /* OpenVPN version 2.x */
        if (strstr(line, match_version))
            retval = TRUE;
    }

out:
    CloseHandle(hStdOutRead);
    CloseHandle(hStdOutWrite);
    return retval;
}

void
DisablePasswordSave(connection_t *c)
{
    if (ShowLocalizedMsgEx(MB_OKCANCEL, TEXT(PACKAGE_NAME), IDS_NFO_DELETE_PASS, c->config_name) == IDCANCEL)
        return;
    DeleteSavedPasswords(c->config_name);
    c->flags &= ~(FLAG_SAVE_KEY_PASS | FLAG_SAVE_AUTH_PASS);
    SetMenuStatus(c, c->state);
}
