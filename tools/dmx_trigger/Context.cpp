/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Context.cpp
 * Copyright (C) 2011 Simon Newton
 */

#include <algorithm>
#include <string>
#include <vector>

#include "tools/dmx_trigger/Context.h"

using std::string;
using std::vector;


/**
 * Delete the context and all associated variables
 */
Context::~Context() {
  m_variables.clear();
}


/**
 * Lookup the value of a variable.
 * @param name the variable name.
 * @param value a pointer to a string to be updated with the value.
 * @returns true if the variable was found, false if it wasn't.
 */
bool Context::Lookup(const string &name, string *value) const {
  VariableMap::const_iterator iter = m_variables.find(name);
  if (iter == m_variables.end())
    return false;
  *value = iter->second;
  return true;
}


/**
 * Update the value of a variable.
 * @param name the variable name
 * @param value the new value
 */
void Context::Update(const string &name, const string &value) {
  m_variables[name] = value;
}


/**
 * Convert this context to a string
 */
string Context::AsString() const {
  vector<string> keys;
  keys.reserve(m_variables.size());

  VariableMap::const_iterator map_iter = m_variables.begin();
  for (; map_iter != m_variables.end(); ++map_iter)
    keys.push_back(map_iter->first);

  sort(keys.begin(), keys.end());

  std::stringstream str;
  vector<string>::const_iterator iter = keys.begin();
  for (; iter != keys.end(); ++iter) {
    if (iter != keys.begin())
      str << ", ";
    map_iter = m_variables.find(*iter);
    str << *iter << "=" << map_iter->second;
  }
  return str.str();
}


/**
 * Stream operator
 */
std::ostream& operator<<(std::ostream &out, const Context &c) {
  return out << c.AsString();
}