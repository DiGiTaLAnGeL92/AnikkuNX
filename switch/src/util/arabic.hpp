#pragma once
#include <string>

/** Prepara un testo con lettere arabe per NanoVG, che non sa collegarle ne' scriverle da destra a sinistra:
 *  sceglie la forma giusta di ogni lettera (iniziale/centrale/finale/isolata) e riordina i tratti RTL.
 *  Un testo senza lettere arabe torna identico. */
std::string shapeArabic(const std::string& utf8);

/** Usata da borealis (Label::setText, vedi scripts/setup.sh). */
std::string brls_shapeArabic(const std::string& utf8);
