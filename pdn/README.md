# LinBPQ Mail for packet.net (linmail-pdn)

This is the LinBPQ mail server, G8BPQ's BBS, running as an app on a [packet.net](https://github.com/packet-net/packet.net) (pdn) node. Your users connect to the BBS over the air as they always have, forwarding with other BBSes works as before, and webmail and the mail management pages open from the pdn control panel.

It's the same mail code LinBPQ runs, unchanged. A small shim in this directory stands in for the LinBPQ node and talks to pdn instead.

## Not John's project

LinBPQ is John Wiseman G8BPQ's work, and this repository is a fork of [his LinBPQ](https://github.com/g8bpq/linbpq). linmail-pdn itself is a packet.net project, not his. If something goes wrong with it, please [open an issue here](https://github.com/packet-net/linmail-pdn/issues) rather than contacting John. If it turns out to be in his code, we'll take it from there.

## Using it

- Install the `pdn-linmail` package from the [releases](https://github.com/packet-net/linmail-pdn/releases) on the machine running pdn, then enable **LinBPQ Mail** in the control panel.
- Moving an existing LinBPQ mailbox across? Follow [MIGRATING.md](MIGRATING.md). Your files carry over unchanged, and you can go back at any time.

Releases are numbered after the LinBPQ version they're built from, then our own release number: `6.0.25.41-pdn1` is our first release on LinBPQ 6.0.25.41. `linmail-pdn --version` shows both.

## How John's updates get here

- The `upstream` branch is an exact copy of John's master. It only ever moves forward, and never has any of our changes in it.
- `main` is John's code plus this `pdn/` directory and our workflows. We never edit John's files, so his releases merge in cleanly.
- Every day a workflow checks John's git server (or his GitHub mirror if that's down). When he has published something new, it moves `upstream` forward, merges it into a branch off `main`, runs all our tests, and opens a pull request with the result.
- Once that pull request is reviewed and merged, the next release is tagged `v<his version>-pdn1`.

## Building and testing

```
make -C pdn                       # needs libjansson-dev, libconfig-dev, zlib1g-dev
pdn/linmail-pdn --version
pdn/tests/ci.sh build fake asan   # the quick suites; pdn/tests/ci.sh alone runs everything CI does
```

## More

- [MIGRATING.md](MIGRATING.md): moving a LinBPQ mailbox to pdn.
- [PLAN.md](PLAN.md): how it works, what's in and out of scope, tests, packaging and the work so far.
- [UPSTREAM-BUGS.md](UPSTREAM-BUGS.md): bugs found in LinBPQ's mail code, and how linmail-pdn works round them.
