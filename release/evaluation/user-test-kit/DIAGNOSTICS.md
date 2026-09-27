# When something fails: what to collect

Collect the items below. Before sending anything, remove personal data.

## 1. The installation check

```bash
cd ~/navier-test/navier-0.3.0-rc2-macos-arm64
bin/navier-ctl doctor --json > ~/navier-test/doctor.json; echo "exit $?" >> ~/navier-test/doctor-exit.txt
cat VERSION > ~/navier-test/version.json
sw_vers > ~/navier-test/macos.txt; df -h ~ >> ~/navier-test/macos.txt
```

## 2. The study folder without the large result fields

The `results.nvr` files hold full result fields (5–150 MB each). They are not needed for a diagnosis, and the record
keeps their SHA-256.

```bash
cd ~/navier-test
tar --exclude='results.nvr' -czf study-for-diagnosis.tar.gz <study folder name>
```

## 3. What you typed and saw

- The exact commands, and the terminal output around the failure. Copy the text; screenshots are harder to read.
- The exit status: run `echo $?` immediately after the failing command.
- With an AI client: its conversation export, if the client can produce one.

## 4. Remove personal data before sending

- Your home folder appears in paths (the folder named after your user account). Replace it, in the text files and inside the
  study folder's JSON files, with `~`:

  ```bash
  grep -rl "$HOME" study-folder-copy | xargs sed -i '' "s#$HOME#~#g"
  ```

  Run this on a copy of the folder, not the original.
- Do not send credentials, access tokens, or files unrelated to the task.
- The task geometry contains nothing personal. If you tried your own CAD files, leave them out unless the facilitator
  agreed.

## 5. Send

Give the items to the facilitator as agreed, together with `FEEDBACK_FORM.md`. `doc/REPORTING_FAILURES.md` in the
package describes a complete failure report.
