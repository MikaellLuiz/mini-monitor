"""Le o refresh token do Antigravity no Gerenciador de Credenciais do Windows
(alvo `gemini:antigravity`). Usado pelos outros scripts; nunca imprime o token.
"""
import ctypes
import re
from ctypes import wintypes

TARGET = "gemini:antigravity"
CRED_TYPE_GENERIC = 1


class FILETIME(ctypes.Structure):
    _fields_ = [("lo", wintypes.DWORD), ("hi", wintypes.DWORD)]


class CREDENTIAL(ctypes.Structure):
    _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD), ("TargetName", wintypes.LPWSTR),
                ("Comment", wintypes.LPWSTR), ("LastWritten", FILETIME),
                ("CredentialBlobSize", wintypes.DWORD), ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)),
                ("Persist", wintypes.DWORD), ("AttributeCount", wintypes.DWORD),
                ("Attributes", ctypes.c_void_p), ("TargetAlias", wintypes.LPWSTR),
                ("UserName", wintypes.LPWSTR)]


def read_blob():
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    ptr = ctypes.POINTER(CREDENTIAL)()
    if not adv.CredReadW(TARGET, CRED_TYPE_GENERIC, 0, ctypes.byref(ptr)):
        raise SystemExit(f"credencial '{TARGET}' nao encontrada (erro {ctypes.get_last_error()})")
    cred = ptr.contents
    raw = bytes(cred.CredentialBlob[i] for i in range(cred.CredentialBlobSize))
    adv.CredFree(ptr)
    return raw


def refresh_token():
    raw = read_blob()
    # O blob pode ser UTF-8 ou UTF-16 (JSON ou texto puro): tenta as duas.
    for enc in ("utf-8", "utf-16-le"):
        try:
            text = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        m = re.search(r"1//[A-Za-z0-9._-]{20,}", text)
        if m:
            return m.group(0)
    raise SystemExit(f"nao achei um refresh token no blob ({len(raw)} bytes)")
