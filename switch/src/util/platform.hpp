#pragma once

namespace platform {
/** true se l'app e' stata avviata in modalita' applet (dall'Album): memoria molto limitata. */
bool isAppletMode();

/** Chi chiede di tenere sveglia la console (niente standby automatico). */
enum AwakeReason { AWAKE_PLAYER = 1, AWAKE_DOWNLOAD = 2 };
/** Attiva/disattiva una ragione: la console resta sveglia finche' almeno una e' attiva. */
void setAwake(AwakeReason reason, bool on);
/** true se c'e' un download in corso (il player non manda la console in standby). */
bool downloadActive();
}  // namespace platform
