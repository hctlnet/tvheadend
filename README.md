<div align="center">

[![Build Status](https://github.com/tvheadend/tvheadend/actions/workflows/build-cloudsmith.yml/badge.svg?branch=master)](https://github.com/tvheadend/tvheadend/actions/workflows/build-cloudsmith.yml)
[![Coverity Scan](https://scan.coverity.com/projects/2114/badge.svg)](https://scan.coverity.com/projects/2114)
[![Github last commit](https://img.shields.io/github/last-commit/tvheadend/tvheadend)](https://github.com/tvheadend/tvheadend)

[![Releases](https://img.shields.io/github/tag/tvheadend/tvheadend.svg?style=flat-square)](https://github.com/tvheadend/tvheadend/releases)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](./LICENSE.md) 
[![GitHub Activity](https://img.shields.io/github/commit-activity/y/tvheadend/tvheadend.svg?label=commits)](https://github.com/tvheadend/tvheadend/commits)

[![Hosted By: Cloudsmith](https://img.shields.io/badge/Packages%20hosted%20by-cloudsmith-blue?logo=cloudsmith&style=flat-square)](https://cloudsmith.io/~tvheadend/repos/tvheadend/packages/)

</div>

Tvheadend
=========

Tvheadend is the leading TV streaming server and Digital Video Recorder for Linux.

![tvheadend front page](https://github.com/tvheadend/tvheadend/raw/master/src/webui/static/img/epg.png)

In this fork
------------

This is a fork of [tvheadend/tvheadend](https://github.com/tvheadend/tvheadend). Its branch `main`
follows upstream `master` and adds what is described here; below this section the README is the
upstream one.

### DAB radio and T2-MI from satellite

  * **DAB and DAB+** in every form it is carried in on a satellite transponder: ETI-NA, MPE and
    TS NI in a PID, and in a GSE stream (that one needs the Neumo driver, see below). A scan finds
    the ensembles (network option *Search DAB ensembles*); each ensemble is a mux with its
    services, which play, record and map to channels like any other. DAB+ audio can be transcoded
    to AAC-LC for clients that do not play it; station logos, programme text and slideshow come
    along. [libdvbdab](https://github.com/hctlnet/libdvbdab) does the DAB part and is built
    together with tvheadend.
  * **T2-MI** and TS piping: the transport streams of DVB-T2 multiplexes that are fed to
    transmitters over satellite. The streams are found in the PMTs of the transponder, each PLP
    is a mux of its own, also when the carrier service is scrambled.

### Instant zapping

Muxes marked for it, and optionally the last used ones, stay tuned with a ring buffer of their
whole transport stream. A channel on such a mux starts from the buffer, close to live, in about
half a second instead of waiting for the next tables, keys and keyframe. An ECM prefetch (camd35)
can keep the keys of chosen services ready. Settings: *Configuration → Stream → Instant Zapping*.

### Neumo driver: several RF inputs, multistream transponders at once, blindscan

This part needs a DVB-S2 card driven by the [Neumo driver](https://github.com/deeptho/neumo-drivers)
and that driver installed. It was developed with a TBS 6909X and with
[our branch of the driver](https://github.com/hctlnet/neumo-drivers), which carries the fixes this
work needed. Without the driver tvheadend works as before; a build with the driver's headers also
runs on other drivers.

  * **Several RF inputs**: each tuner of such a card can use each of its RF inputs. Every input
    is a frontend with its own satellite configuration, and the tuners share an input.
  * **Multistream transponders at once**: one tuner delivers all input streams (ISI) of a
    transponder together. A scan finds the streams; services of several of them play at the same
    time on that one tuner. GSE streams are read too: what they carry is shown, and DAB in GSE
    plays.
  * **Blindscan**: the spectrum of a DVB-S network, the carriers in it next to the known
    transponders, and a tune at a carrier that takes its parameters from the demodulator, from a
    dialog in the mux tree. Also blind tuning for the normal scan, and for narrow carriers.

### Other additions

  * **For HTSP clients**: component tags, subtitle and teletext details, audio type and receiver
    mix, the DVB ids of a channel, internal channel tags on request, HbbTV stream events
    (DSM-CC), and a scrambled channel without a key starts with "No access" instead of silence.
    [TVF](https://github.com/hctlnet/tvfrontend), a client for LG webOS TVs, uses them.
  * **Satellite setup**: Unicable groups for tuners that share a cable (one or several masters
    send for the others), a correction for the local oscillators of an LNB, an external command
    for a polarizer; a NIT entry that only repeats a known transponder is not scanned as a second
    one, and a NIT does not overwrite tuning a lock confirmed.

### MH EPG and fixes

  * **MH EPG**: the EPG some operators send in a DSM-CC object carousel (for example Sky Germany,
    up to 13 days), as an EIT grabber module.
  * **Fixes**, among them for capmt with OSCam (ECM flush, CSA-ALT, message order) and for the
    local language codes qaa..qtz.

### Why sub-muxes, and what changed in the mux list

A DAB ensemble, a T2-MI stream and an input stream of a multistream transponder have one thing in
common: they are carried inside a transponder and have no tuning of their own. Here each of them is
a **sub-mux** of its transponder. It is a mux like any other - with services, scan, EPG and
channels - so everything after the mux works unchanged: descrambling, recording, streaming, instant
zapping. But the tuning stays in one place: the NIT, a blindscan or the user change the transponder,
and its sub-muxes follow.

A sub-mux takes its data from the transponder the way a client does. So the transponder keeps one
tuner for its own services and for all sub-muxes together, they can be nested (a transponder, an
input stream in it, a DAB ensemble in that), a carrier can be descrambled first, and it works on
every input that delivers the transponder, not only with one driver.

Muxes with a hierarchy do not fit a flat list. *Configuration → DVB Inputs → Muxes* is therefore a
tree: networks, their transponders with the tuning, the sub-muxes carried in them, and the services
of each mux. A row can be scanned and played, a scan of a transponder scans what is in it, the PID
or address of a sub-mux is edited in its cell, and a filter finds muxes and services. *List view*
still shows the flat grid of all muxes as upstream has it.

To build, see below; libdvbdab is downloaded and built by `make` (it needs a C++ compiler and
cmake 3.20 or later, `./configure --disable-dvbdab` leaves DAB out).

To try the fork without installing it there is a container image that needs only the DVB devices
and a copy of a configuration: see
[Quick test image of this fork](README.Docker.md#quick-test-image-of-this-fork).

---

*From here on this is the README of upstream Tvheadend, unchanged.*

It supports the following inputs:

  * ATSC
  * DVB-C(2)
  * DVB-S(2)
  * DVB-T(2)
  * HDHomeRun
  * IPTV
    * UDP
    * HTTP
  * SAT>IP
  * Unix Pipe

It supports the following outputs:

  * HTSP (native protocol)
  * HTTP
  * SAT>IP

Documentation
-------------

Tvheadend documentation can be found here: [https://docs.tvheadend.org](https://docs.tvheadend.org).

Support
-------

Please triage issues and ask questions in the forum: [https://tvheadend.org](https://tvheadend.org) or use the `#hts` IRC channel on Libera.Chat to speak with project staff: [https://web.libera.chat/#hts](https://web.libera.chat/#hts).

Please report triaged bugs via GitHub Issues. 

Building for Linux
------------------

First you need to configure:

	$ ./configure

If build dependencies are missing the configure script will complain or attempt
to disable optional features.

To build the binary:

	$ make

After compiling the Tvheadend binary is in the `build.linux` directory.

To run the Tvheadend binary:

	$ ./build.linux/tvheadend

Settings are stored in `$HOME/.hts/tvheadend`.

To install the newly compiled Tvheadend binary and associated files onto your system:

	$ sudo make install

Running on Linux
----------------

Instructions for popular distributions are in our public [documentation](https://docs.tvheadend.org/documentation/installation/linux).

Running in Docker
-----------------

Running in Docker can be as simple as:

	$ docker run --rm ghcr.io/tvheadend/tvheadend:latest

See [README.Docker.md](README.Docker.md) for more details.
