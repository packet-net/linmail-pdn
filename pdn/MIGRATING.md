# Moving your LinBPQ mailbox to pdn

LinBPQ Mail on pdn (linmail-pdn) is the same mail server you run in LinBPQ today, built on its own and connected to a pdn node. Your messages, users, forwarding partners and connect scripts carry straight across. You copy your files over, tell it which pdn ports your connect scripts mean, and you're done.

## Before you start

- Install the `pdn-linmail` package on the machine running pdn. It lands in `/usr/share/packetnet/apps/linmail`.
- In the pdn control panel, open **Apps**, find **LinBPQ Mail** and set its **callsign**. Use your BBS's callsign from bpq32.cfg (the call on your `APPLICATION` line, or `BBSCALL`). Don't enable it yet.
- Stop LinBPQ, or at least its mail, so nothing is writing to the files while you copy them.

## Copy your files

Copy these from your LinBPQ directory into `/var/lib/packetnet/apps/linmail/`:

- `linmail.cfg` (settings, users and forwarding partners)
- `DIRMES.SYS` (the message list) and the `Mail/` folder (the messages themselves)
- `WFBID.SYS` (BIDs already seen), `WP.SYS` (white pages)
- `BADWORDS.SYS` and `INTRCPT.APS`, if you have them
- `BPQBBSUsers.dat`, if your LinBPQ is old enough to still have one

Then make them belong to pdn:

```
sudo chown -R packetnet:packetnet /var/lib/packetnet/apps/linmail
```

Nothing in these files needs editing.

## Tell it about your ports

Your connect scripts say things like `C 2 GB7XYZ`, where `2` is a LinBPQ port number. pdn names its ports instead (`vhf`, `hf` and so on). Create `/var/lib/packetnet/apps/linmail/linmail-pdn.conf` with a port map:

```
portmap = 1=vhf, 2=hf
```

The same numbers pick the ports for mail-for beacons (the `UIPort` settings in `linmail.cfg`). There's a commented example with every setting in `/usr/share/packetnet/apps/linmail/linmail-pdn.conf.example`.

## Start it

Enable **LinBPQ Mail** in the control panel. pdn starts it, and it answers on the callsign you set. Stations can connect to that call directly, or type `MAIL` at the node prompt. Webmail and the mail management pages open from the app's entry in the panel.

Webmail uses your pdn login instead of the BBS password. If your pdn username is a callsign with an account on the BBS, you get that mailbox. A pdn username that's a callsign with no BBS account yet gets a message saying so: connect to the BBS over the air once to create it.

Node admins always get the management pages, acting as the BBS sysop, even with no BBS account of their own. An admin whose username isn't a callsign also gets the sysop's mailbox in webmail.

## What's different from LinBPQ

- **Connect scripts:** plain `C port CALL` lines work, including multi-hop scripts where later lines are typed at the far node. pdn can't do digipeated connects (`C CALL V DIGI`), NET/ROM alias connects, or BPQ-only node commands (`NC`, `ATTACH`, `RADIO`, `MCAST`). Those lines fail, so an `ELSE` block in the script still runs.
- **Not included:** the SMTP, POP3 and NNTP servers. They stay off even if `linmail.cfg` turns them on, and linmail-pdn logs a line saying so. MQTT, the packet map, Winlink session reporting and multicast mail aren't included either.
- **UI beacons** go out without a digipeater path, and the BBS doesn't answer header resync requests heard on air.
- **Logins:** BPQ's own web login isn't used. The pages only open through the pdn control panel.

## Going back

Your files aren't converted, so you can copy them back into your LinBPQ directory at any time and carry on there.
