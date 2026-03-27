#!/usr/bin/env python

# Authority source: https://inside-docupedia.bosch.com/confluence2/x/jADAEg

import requests
from pathlib import Path
import hashlib
import subprocess


class Cert:
    def __init__(self, url, filename, fingerprint_sha1, fingerprint_sha256, filehash_sha256):
        self.url = url
        self.filename = filename
        self.fingerprint_sha1 = fingerprint_sha1.lower()
        self.fingerprint_sha256 = fingerprint_sha256.lower()
        self.filehash_sha256 = filehash_sha256.lower()

    def download_to_folder(self, folder: Path):
        response = requests.get(self.url)
        response.raise_for_status()
        target_path = folder / self.filename
        with target_path.open("wb") as f:
            raw = response.content
            f.write(raw)
        self.verify_file(target_path)

    def verify_file(self, path: Path):
        if not path.exists():
            raise FileNotFoundError(f"Certificate file {path} does not exist.")

        with path.open("rb") as f:
            raw = f.read()
            sha256 = hashlib.sha256(raw).hexdigest()
            if sha256 != self.filehash_sha256:
                raise ValueError(f"Hash mismatch for {self.filename}: expected {self.filehash_sha256}, got {sha256}")

        # verify fingerprints
        _check_fingerprint(path, "sha1", self.fingerprint_sha1)
        _check_fingerprint(path, "sha256", self.fingerprint_sha256)


def _check_fingerprint(cert_path: Path, type: str, expectation: str) -> str:
    cmd = ["openssl", "x509", "-in", str(cert_path), "-noout", "-" + type, "-fingerprint"]
    # print("RUNNING:", " ".join(cmd))
    result = subprocess.run(cmd, check=True, text=True, capture_output=True)
    fingerprint = result.stdout.strip().split("=")[1].replace(":", "").lower()
    if fingerprint != expectation:
        raise ValueError(f"{type.upper()} Fingerprint mismatch for {cert_path.name} {type}: expected '{expectation}', got '{fingerprint}'")
    return fingerprint


CERTIFICATES = [
    Cert(
        url="https://inside-docupedia.bosch.com/confluence2/download/attachments/314572940/rb_rootca_ecc_g01.crt?version=2&modificationDate=1741273682000&api=v2",
        filename="rb_rootca_ecc_g01.crt",
        fingerprint_sha1="e2193de57aab4c0c9e9bbcbe212e28360cf205cb",
        fingerprint_sha256="80630d1ffdac69622b039d490e910f5e7130f54606a6d73c9eed9522d06ef869",
        filehash_sha256="77A022260927DB365D565EF58FA9AF78D3C864DBFAC68400E3896FEBE8D0CCC8",
    ),
    Cert(
        url="https://inside-docupedia.bosch.com/confluence2/download/attachments/314572940/rb_rootca_rsa_g01.crt?version=1&modificationDate=1741339361000&api=v2",
        filename="rb_rootca_rsa_g01.crt",
        fingerprint_sha1="8e468cb78e150cdb2473536693e14732b5075919",
        fingerprint_sha256="befea4b1f8754f18c449a0d550ffe76daba609f410b65fed1bb4064aa6d9a306",
        filehash_sha256="46D26CE24971172304C94BD5D5308866DC394391EDEF3AF55D20282E8A2F1AEC",
    ),
]


def fetch_certificates():
    cert_folder = Path("./certs")
    cert_folder.mkdir(exist_ok=True)
    for cert in CERTIFICATES:
        cert.download_to_folder(cert_folder)


def check_installed():
    copy_to = Path("/usr/local/share/ca-certificates/bosch")
    if not copy_to.exists():
        print(f"Certificate copy folder {copy_to} does not exist: sudo mkdir -p {copy_to}")
    deploy_to = Path("/etc/ssl/certs")
    for cert in CERTIFICATES:
        c = copy_to / cert.filename
        if not c.exists():
            print(f"Certificate {c} is not installed: sudo cp {Path('./certs')}/* {copy_to}/")
        else:
            cert.verify_file(c)
        d = deploy_to / (cert.filename.removesuffix(".crt") + ".pem")
        if not d.exists():
            print(f"Certificate {d} is not deployed: sudo update-ca-certificates")
        else:
            cert.verify_file(d)


if __name__ == "__main__":
    fetch_certificates()
    check_installed()
