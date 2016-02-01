# AirDC++ Web Client

AirDC++ Web Client is a cross-platform file sharing client for Advanced Direct Connect network. It has a web-based user interface that can be accessed with a web browser.

## Table of contents

 * [Installation](#installation)
 * [Reporting issues](#reporting-issues)
 * [Feature requests](#feature-requests)

## Installation

[Installation guide](/INSTALL.md)

## Reporting issues

Use the bug tracker of this project for all bug reports. 

The following information should be included for all reports:

* Instructions for reproducing the issue (if possible)
* Client and UI versions (copy from Settings -> About)

### UI-related issues

If the UI behaves incorrectly, you should open the console of your browser and check if there are any errors. It's even better if you manage to reproduce the issue while the console is open, as it will give more specific error messages. Include the errors in your bug report.

Other useful information:

* Browser version and information whether the issue happens with other browser as well
* Screenshots or video about the issue

### Client crash

Include all text from the generated crash log to your bug report. The log is located at ``/home/<username>/.airdc++/exceptioninfo.txt``.

### Client freeze/deadlock

Note that you should first confirm whether the client has freezed and the issue isn't in the UI (try opening the UI in a new tab).

Install the ``gdb`` package before running the following commands.

```
$ cat ~/.airdc++/airdcppd.pid
[number]
gdb
attach [number]
thread apply all bt full
```

Save the full output to a file and attach it to your bug report.

## Feature requests

The issue tracker can be used for posting feature requests as well.
