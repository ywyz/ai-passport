#!/usr/bin/env python3
"""Create a private MiMo ASR config; never accept an API key as an argument."""
import argparse
import getpass
import json
import os
from pathlib import Path
import re


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",required=True)
    parser.add_argument("--daily-seconds",type=int,default=300)
    args=parser.parse_args()
    if not 1<=args.daily_seconds<=3600: parser.error("daily-seconds must be 1..3600")
    path=Path(args.output)
    if path.exists() or path.is_symlink(): parser.error("output already exists")
    key=getpass.getpass("MiMo pay-as-you-go API key (hidden): ")
    if not re.fullmatch(r"[!-~]{16,256}",key): parser.error("invalid API key format")
    path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    with open(path,"x",encoding="utf-8",opener=lambda p,f:os.open(p,f,0o600)) as output:
        json.dump({"api_key":key,"daily_seconds":args.daily_seconds},output)
    print("Private speech config created. Enable only on the companion server; never flash this key.")


if __name__=="__main__": main()
