#pragma once

// Config-mode web server for Bahnbrett.
// Starts an AP named "Bahnbrett-Config" and serves a web form at 10.254.0.1.
// On save, persists config to NVS and reboots into normal mode.

// Returns true if the user saved a new config (caller should reboot).
// Returns false if the user cancelled or the timeout expired (caller should sleep).
bool bahnbrett_config_web_start(void);
