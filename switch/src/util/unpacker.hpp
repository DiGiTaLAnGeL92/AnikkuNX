#pragma once

#include <string>

/** Decodifica degli script compressi con il "packer" di Dean Edwards: eval(function(p,a,c,k,e,d){...}). */
namespace unpacker {

bool detect(const std::string& script);
/** Decodifica tutti i blocchi compressi trovati e li unisce con uno spazio ("" se nessuno). */
std::string unpackAndCombine(const std::string& script);

}  // namespace unpacker
