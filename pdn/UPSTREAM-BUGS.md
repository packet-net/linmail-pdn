# Upstream bugs found while building linmail-pdn

These are in LinBPQ's own mail code, so they affect LinBPQ as well as linmail-pdn. linmail-pdn works round each one without editing the upstream files (details below), so nothing here blocks it. They're written up so Tom can decide whether to pass them on to John. Line numbers are from this fork's master at b2d4b3c. Each crash was reproduced with AddressSanitizer.

## 1. Webmail form posts read fields that weren't sent

- **Where:** `WebMail.c` line 2573 in `SendTemplateSelectScreen` (`strlen(WebMail->BID)`), then `_strdup` of `To`, `Subject` and `Body` at lines 2584 to 2588. `SaveNewMessage` does the same at line 2703.
- **Trigger:** any signed-in webmail user posting `/WebMail/GetTemplates` (or `/WebMail/EMSave`) with a form that leaves out `BID` (or `To`, `Subj`, `Msg`). The field pointer is still NULL, so `strlen(NULL)` crashes the process.
- **Suggested fix:** treat a missing field as empty, for example `if (WebMail->BID == NULL) WebMail->BID = _strdup("");` for each of the four before they're used.
- **linmail-pdn:** `pdnweb.c` sets any missing field to an empty string before a webmail form post reaches the upstream handler.

## 2. Template folder and form numbers aren't range checked

- **Where:** `WebMail.c` line 2446 (`Dir = HtmlFormDirs[DirNo]` in the `/WebMail/GetList/` handler, then `Dir->Dirs[SubDirNo]`), line 3127 in `GetPage` (`HtmlFormDirs[DirNo]`, read before its own `DirNo == -1` check, then `Dir->Dirs[SubDirNo]` and `Dir->Forms[FileNo]`), and line 2594 in `SendTemplateSelectScreen` (`HtmlFormDirs[0]`, NULL when no template folders are installed).
- **Trigger:** `GET /WebMail/GetList/100000?<key>`, `GET /WebMail/GetPage/0,99?<key>`, or opening the template list on a BBS with no `Standard_Templates` folder. Any webmail user can do this. It reads out of bounds and usually crashes.
- **Suggested fix:** check `0 <= DirNo < FormDirCount`, `SubDirNo < Dir->DirCount` and `FileNo < Dir->FormCount` before indexing, check `FormDirCount > 0` before using `HtmlFormDirs[0]`, and do the `-1` check in `GetPage` before the lookup.
- **linmail-pdn:** `pdnweb.c` checks the folder, subfolder and form numbers (and that any template folders exist) and answers 400 instead of passing a bad request on.

## 3. A bare `ELSE` in a connect script is read past its end

- **Where:** `BBSUtilities.c` line 8414 in `ProcessBBSConnectScript`: `_memicmp(&Cmd[5], "DELAY", 5)`.
- **Trigger:** a forwarding connect script with an `ELSE` line (the usual form, with no `DELAY`), after a failed connect. `Cmd` is the 5 byte string `"ELSE"`, so the comparison reads 5 bytes beyond it. This usually goes unnoticed, but it's a heap overread on every failed forward attempt that has an alternative.
- **Suggested fix:** `if (strlen(Cmd) > 5 && _memicmp(&Cmd[5], "DELAY", 5) == 0)`.
- **linmail-pdn:** `linmail-pdn.c` (`PadElseLines`) gives every short `ELSE` line enough zeroed room that the check stays inside its own memory. It does this once per line, including lines set later from the web pages or the `FWD` command.

## 4. Smaller ones

- **`HTTPcode.c` line 323, `UndoTransparency`:** a `%` at the end of a form value, or one not followed by two hex digits, makes it read and write past the end of the string. linmail-pdn has its own copy that leaves such a `%` alone. Suggested fix: decode `%xx` only when both following characters are hex digits.
- **`BBSUtilities.c` line 279, `FilesNames[4][100]`:** the log file name is copied in with `strcpy` at line 307, so a log directory path longer than about 70 characters overflows it and aborts under glibc's fortify checks. linmail-pdn refuses such a path at start-up. Suggested fix: size the buffer `MAX_PATH`, or use `strncpy`.
