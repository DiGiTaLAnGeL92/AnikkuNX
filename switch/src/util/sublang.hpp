#pragma once

#include <string>
#include <vector>

/** Lingua preferita dei sottotitoli (Impostazioni > Riproduzione). */
namespace sublang {

/** Codici selezionabili nelle impostazioni, in ordine: "auto" (lingua della console), lingue, "off". */
const std::vector<std::string>& choices();

/** Codice effettivo: la scelta dell'utente, oppure la lingua della console se "auto". "off" = nessun sottotitolo. */
std::string preferred();

/** Nomi con cui i siti e le tracce indicano la lingua (es. "it" -> it, ita, italian, italiano). */
std::vector<std::string> keys(const std::string& code);

/** true se l'etichetta di una traccia ("English", "ita", "pt-BR"...) corrisponde alla lingua. */
bool matches(const std::string& trackLang, const std::string& code);

/** Valore per l'opzione "slang" di mpv: lingua preferita, poi inglese. */
std::string mpvSlang();

}  // namespace sublang
