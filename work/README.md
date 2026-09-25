# work/

Local scratch space, **gitignored** except for this file.

Put anything here that is generated locally and must never be committed:

* `dfb/` — the DirectFB 1.4.16 install staged by
  [`tools/build-directfb`](../tools/build-directfb/README.md)
* `rbx3-run.tgz` — the chroot built by
  [`scripts/build-chroot.sh`](../scripts/build-chroot.sh)
* extracted firmware trees, patched players, device captures, build logs, …

No vendor firmware, decryption keys, `rbp`/`rb` binary or Engine OS file
belongs in the repository. `.gitignore` keeps this directory out, but keep the
release tree itself clean too.
