#!/usr/bin/env python3
"""Builds the root-certificate bundle for the Bahnbrett device build.

The script reads the Mozilla CA list in PEM form, keeps the curated roots
below, and writes lib/bahnbrett/src/bahnbrett_root_bundle.cpp. The bundle
format matches the parser in the WiFiClientSecure library: a big-endian
certificate count, then per certificate the name length, the key length, the
DER subject name, and the DER public key. The entries are sorted by name
because the parser runs a binary search.

Use:
    python3 lib/bahnbrett/tools/gen_root_bundle.py [--pem PATH]

The script fetches the list from https://curl.se/ca/cacert.pem when no path
is given. See ADR 0040.
"""

import argparse
import base64
import binascii
import sys
import urllib.request

K_CACERT_URL = "https://curl.se/ca/cacert.pem"
K_OUTPUT = "src/bahnbrett_root_bundle.cpp"

# The curated roots. The set covers the three departures APIs (Let's Encrypt
# and DigiCert sign them today) and the common public CAs, so a remote-config
# URL on a public host verifies as well. A self-hosted host with a private
# CA needs a pinned certificate in the config; the config page documents
# the steps.
K_ROOT_NAME_PARTS = (
    "ISRG Root",
    "GTS Root",
    "GlobalSign Root",
    "DigiCert Global Root",
    "AAA Certificate Services",
    "USERTrust",
    "Sectigo Public Server Authentication Root",
    "COMODO ECC Certification Authority",
    "Amazon Root",
    "D-TRUST Root",
    "T-TeleSec GlobalRoot",
    "Buypass Class",
)


def fail(message):
    """Prints a full-sentence error and exits with a nonzero code."""
    print(f"The bundle generator fails: {message}", file=sys.stderr)
    sys.exit(1)


def load_pem(path):
    """Reads the PEM text from a path or the URL."""
    if path:
        try:
            # The list holds UTF-8 CA names, so the decoder replaces any
            # broken byte instead of failing the whole run.
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                return handle.read()
        except OSError as err:
            fail(f"the script cannot open {path} because {err}. "
                 "Check the path and run the script again.")
    try:
        with urllib.request.urlopen(K_CACERT_URL, timeout=30) as response:
            return response.read().decode("utf-8", "replace")
    except OSError as err:
        fail(f"the script cannot fetch {K_CACERT_URL} because {err}. "
             "Download the file and pass it with --pem.")


def split_certs(pem_text):
    """Yields (name, der_bytes) for every certificate in the PEM text."""
    lines = pem_text.splitlines()
    index = 0
    while index < len(lines):
        if not lines[index].startswith("-----BEGIN CERTIFICATE-----"):
            index += 1
            continue
        # The name sits above the ==== separator line.
        name_index = index - 1
        while name_index >= 0 and lines[name_index].startswith("="):
            name_index -= 1
        if name_index < 0:
            fail("the PEM file holds a certificate without a name line. "
                 "Use the unmodified list from curl.se.")
        name = lines[name_index].strip()
        end = index + 1
        body = []
        while end < len(lines) and not lines[end].startswith(
                "-----END CERTIFICATE-----"):
            body.append(lines[end])
            end += 1
        if end >= len(lines):
            fail(f"the certificate '{name}' has no END line. "
                 "Use the unmodified list from curl.se.")
        try:
            der = base64.b64decode("".join(body))
        except binascii.Error as err:
            fail(f"the certificate '{name}' is not valid base64 because "
                 f"{err}. Use the unmodified list from curl.se.")
        yield name, der
        index = end + 1


def read_tlv(der, pos):
    """Returns (value_start, value_end, tlv_end, tag) of the TLV at pos."""
    if pos + 2 > len(der):
        fail("the certificate ends inside a DER header.")
    tag = der[pos]
    length = der[pos + 1]
    header = 2
    if length & 0x80:
        count = length & 0x7F
        if pos + 2 + count > len(der):
            fail("the certificate ends inside a DER length field.")
        length = int.from_bytes(der[pos + 2:pos + 2 + count], "big")
        header = 2 + count
    value_start = pos + header
    value_end = value_start + length
    if value_end > len(der):
        fail("the certificate ends inside a DER value.")
    return value_start, value_end, value_end, tag


def children(der, value_start, value_end):
    """Yields (tlv_start, tlv_end, tag) of every child in the value region."""
    pos = value_start
    while pos < value_end:
        _, _, tlv_end, tag = read_tlv(der, pos)
        yield pos, tlv_end, tag
        pos = tlv_end


def extract_name_and_key(der, name):
    """Returns the DER subject name and the DER public key of a cert."""
    value_start, value_end, _, tag = read_tlv(der, 0)
    if tag != 0x30:
        fail(f"the certificate '{name}' does not start with a DER sequence.")
    kids = list(children(der, value_start, value_end))
    if not kids or kids[0][2] != 0x30:
        fail(f"the certificate '{name}' holds no tbsCertificate sequence.")
    tbs_start = kids[0][0]
    tbs_value_start, tbs_value_end, _, _ = read_tlv(der, tbs_start)
    fields = []
    for field_start, field_end, field_tag in children(
            der, tbs_value_start, tbs_value_end):
        if field_tag == 0xA0 and not fields:
            # The optional version field carries a context tag; skip it.
            continue
        fields.append((field_start, field_end, field_tag))
    # serial, signature, issuer, validity, subject, subjectPublicKeyInfo
    if len(fields) < 6:
        fail(f"the certificate '{name}' holds too few tbs fields.")
    subject_start, subject_end, _ = fields[4]
    spki_start, spki_end, _ = fields[5]
    name_der = der[subject_start:subject_end]
    key_der = der[spki_start:spki_end]
    if not 10 < len(name_der) < 512:
        fail(f"the certificate '{name}' holds an implausible subject name.")
    if not 64 < len(key_der) < 1200:
        fail(f"the certificate '{name}' holds an implausible public key.")
    return name_der, key_der


def build_bundle(selected):
    """Builds the binary bundle from sorted (name_der, key_der) entries."""
    entries = sorted(selected, key=lambda entry: entry[0])
    blob = len(entries).to_bytes(2, "big")
    for name_der, key_der in entries:
        blob += len(name_der).to_bytes(2, "big")
        blob += len(key_der).to_bytes(2, "big")
        blob += name_der
        blob += key_der
    return blob


def write_cpp(blob, root_names, count):
    """Writes the C++ source with the bundle bytes."""
    lines = []
    for offset in range(0, len(blob), 16):
        chunk = blob[offset:offset + 16]
        hex_bytes = ", ".join(f"0x{byte:02X}" for byte in chunk)
        lines.append(f"    {hex_bytes},")
    body = "\n".join(lines)
    names_comment = "\n".join("//   " + name for name in root_names)
    text = f"""// Root-certificate bundle for TLS verification; see ADR 0040.
//
// The generator in lib/bahnbrett/tools/gen_root_bundle.py builds this file
// from the Mozilla CA list ({K_CACERT_URL}). The bundle holds the {count}
// public roots that the device trusts:
{names_comment}
//
// Regenerate the file with:
//   python3 lib/bahnbrett/tools/gen_root_bundle.py
// The format matches the parser in the WiFiClientSecure library: a
// big-endian certificate count, then per certificate the name length, the
// key length, the DER subject name, and the DER public key. The entries are
// sorted by name because the parser runs a binary search.

#include <cstdint>

namespace bahnbrett {{

// The bundle lives in flash through const. The linker keeps the array
// because the HTTP client references it under BAHNBRETT_TLS_BUNDLE.
extern const uint8_t kBahnbrettRootBundle[] = {{
{body}
}};

}}  // namespace bahnbrett
"""
    with open(K_OUTPUT, "w", encoding="ascii") as handle:
        handle.write(text)


def main():
    parser = argparse.ArgumentParser(
        description="Builds the Bahnbrett root-certificate bundle.")
    parser.add_argument(
        "--pem", help="path to the Mozilla CA list; the script fetches the "
        "list from curl.se when no path is given")
    args = parser.parse_args()

    pem_text = load_pem(args.pem)
    selected = []
    root_names = []
    seen = set()
    for name, der in split_certs(pem_text):
        if not any(part in name for part in K_ROOT_NAME_PARTS):
            continue
        if name in seen:
            continue
        seen.add(name)
        name_der, key_der = extract_name_and_key(der, name)
        selected.append((name_der, key_der))
        root_names.append(name)
    if not selected:
        fail("the curated root list matches no certificate in the list. "
             "Check K_ROOT_NAME_PARTS against the names in the list.")

    blob = build_bundle(selected)
    write_cpp(blob, root_names, len(selected))
    print(f"The generator writes {len(selected)} roots and {len(blob)} "
          f"bytes into {K_OUTPUT}.")


if __name__ == "__main__":
    main()
