"""Gera data/cert/x509_crt_bundle.bin (CAs raiz da Mozilla, via certifi) no formato
do esp_crt_bundle do Arduino-ESP32:

  [n: u16 BE] e, para cada CA (ordenadas pelo subject DER, para a busca binaria):
  [len_subject: u16 BE][len_chave: u16 BE][subject DER][chave publica DER]

Uso: .venv\\Scripts\\python tools\\gen_ca_bundle.py
Rodar de novo de tempos em tempos (ou ao atualizar o certifi) para renovar as CAs.
"""
import os
import struct

import certifi
from cryptography import x509
from cryptography.hazmat.primitives import serialization

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "cert", "x509_crt_bundle.bin")

with open(certifi.where(), "rb") as f:
    certs = x509.load_pem_x509_certificates(f.read())

entries = {}
for c in certs:
    subject = c.subject.public_bytes()
    key = c.public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    entries[subject] = key  # subject repetido: fica a ultima versao

data = bytearray(struct.pack(">H", len(entries)))
for subject in sorted(entries):
    key = entries[subject]
    data += struct.pack(">HH", len(subject), len(key)) + subject + key

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as f:
    f.write(data)
print(f"{len(entries)} CAs, {len(data)} bytes -> {os.path.relpath(OUT, ROOT)}")
