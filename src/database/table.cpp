/* Driller, a data extraction program
 * Copyright (C) 2005-2006 John Millikin
 *
 * table.cpp
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <errno.h>
#include <sstream>
#include "database.h"
#include "misc.h"

#ifdef __APPLE__
  #ifndef __unix
    #define __unix
  #endif
#endif

// For mmap
#ifdef __unix
  #include <sys/types.h>
  #include <sys/mman.h>
  #include <fcntl.h>

// For Windows versions of mmap
#elif WIN32
  #include <windows.h>
#endif

namespace Driller {

Table::Table(
  const std::string& _friendly_name,
  const std::string& _file_name,
  const unsigned int _data_offset,
  const unsigned int _row_length) throw ():

  friendly_name(_friendly_name),
  file_name(_file_name),
  data_offset(_data_offset),
  row_length(_row_length)
  {}

Table::Table(xmlNode* node) throw (
  Errors::FileParseError,
  Errors::MissingAttributeError,
  Errors::DuplicateEnumID) {

  friendly_name = get_xml_node_attribute(node, "name");
  file_name = get_xml_node_attribute(node, "file");
  std::string raw_data_offset = get_xml_node_attribute(node, "data_offset");
  std::string raw_row_length = get_xml_node_attribute(node, "row_length");

  // Convert raw data into something usable

  std::stringstream strstream;

  strstream << raw_data_offset;
  strstream >> data_offset;

  if (strstream.fail()) {
    throw Errors::InvalidAttributeError("", "data_offset", raw_data_offset);
  }

  strstream.clear();
  strstream << raw_row_length;
  strstream >> row_length;

  if (strstream.fail()) {
    throw Errors::InvalidAttributeError("", "row_length", raw_row_length);
  }

  // Convert any columns in the node
  xmlNode* column_node = node->children;

  for (; column_node; column_node = column_node->next){
    if (column_node->type == XML_ELEMENT_NODE){
      Column col(column_node);

      if (col.get_type() == COLUMN_VARSTRING){
        row_length = 0;
      }

      columns.push_back(col);
    }
  }
}

Table::~Table() throw () {
  clear();
}

unsigned int Table::column_count() const throw(){
  return static_cast<unsigned int>(columns.size());
}

void Table::clear() throw () {
  columns.clear();
  set_name("");
  set_file_name("");
  set_data_offset(0);
  set_row_length(1);
}

void Table::set_name(const std::string& _name) throw(){
  friendly_name = _name;
}

std::string Table::get_name() const throw(){
  return friendly_name;
}

void Table::set_file_name(const std::string& _file_name) throw(){
  file_name = _file_name;
}

std::string Table::get_file_name() const throw(){
  return file_name;
}

void Table::set_data_offset(const unsigned int _offset) throw(){
  data_offset = _offset;
}

unsigned int Table::get_data_offset() const throw(){
  return data_offset;
}

void Table::set_row_length(const unsigned int _row_length) throw(){
  row_length = _row_length;
}

unsigned int Table::get_row_length() const throw(){
  return row_length;
}

Column& Table::column_at(const unsigned int index) throw (){
  return columns.at(index);
}

const Column& Table::column_at(const unsigned int index) const throw (){
  return columns.at(index);
}

void Table::add_column(const Column& column) throw (){
  columns.push_back(column);
}

void Table::remove_column(const unsigned int index) throw (){
  columns.erase(columns.begin() + index);
}

std::vector<Column> Table::get_columns() const throw(){
  return columns;
}

const ResultSet* Table::extract_data(const unsigned int row_limit) const
  throw (Errors::FileReadError){

  ExtractionState* state = load_data();

  // Holds pointers to the start of each row
  const uint8** row_locations;

  // Column count
  const unsigned int column_count = static_cast<const unsigned int>(
    columns.size());

  // Calculate how many rows will be needed
  unsigned int row_count = 0;

  // If there are no variable-width columns
  if (row_length > 0){
    row_count = (state->data_length - data_offset) / row_length;

    // Clamp the row count, if needed
    if (row_limit)
      if (row_count > row_limit)
        row_count = row_limit;

    // Allocate memory for the row location array
    row_locations = new const uint8*[row_count];

    // Calculate the location of each row, and place it into the location array
    for (unsigned int row = 0; row < row_count; row++){
      row_locations[row] = state->data + data_offset + (row_length * row);
    }
  }

  // If there are variable-width columns
  else {
    // FIXME: Dentrix specific
    row_count = 0;

    // First pass to determine the row count
    uint32 current_offset = data_offset;
    while (current_offset < state->data_length){
      ++row_count;
      current_offset += Column::get_uint32(state->data + current_offset + 2);
    }

    // Clamp the row count, if needed
    if (row_limit)
      if (row_count > row_limit)
        row_count = row_limit;

    // Allocate space for the location array
    row_locations = new const uint8*[row_count];

    // Second pass, to insert row locations into the location array
    current_offset = data_offset;
    for (unsigned int row = 0; current_offset < state->data_length &&
      row < row_count; row++){

      row_locations[row] = state->data + current_offset;
      current_offset += Column::get_uint32(state->data + current_offset + 2);
    }
  }

  ResultSet* result = new ResultSet(*this, row_count, column_count);
  unsigned int format_buffer_size = 30;
  char* format_buffer = new char[format_buffer_size];

  // Run through the data, extracting rows into a ResultSet
  std::vector<Column>::const_iterator column_iter = columns.begin();
  for (unsigned int col_idx = 0; col_idx < column_count; col_idx++){
    const Column& column = *column_iter;

    for (unsigned int row_idx = 0; row_idx < row_count; row_idx++){
      result->set_cell(row_idx, col_idx,
        column.extract_data(row_locations[row_idx], format_buffer, format_buffer_size)
      );
    }

    ++column_iter;
  }

  delete [] format_buffer;
  delete [] row_locations;

  unload_data(state);
  return result;
}

Table::ExtractionState* Table::load_data() const throw (Errors::FileReadError){
  std::string full_file_name = Database::get_data_path() + "/" + file_name;
  ExtractionState* state = NULL;

// On UNIX-based systems, mmap the file
#ifdef __unix
  int fd = open(full_file_name.c_str(), O_RDONLY, 0);

  if (fd < 0){
    throw Errors::FileReadError(full_file_name, errno);
  }

  state = new ExtractionState;
  state->data_length = lseek(fd, 0, SEEK_END);

  state->data = static_cast<uint8*>(
    mmap(0, state->data_length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0));

  close(fd);

// On Windows, use the equivalent Win32 API functions
#elif WIN32
  HANDLE file_handle = CreateFile(full_file_name.c_str(), GENERIC_READ,
    FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);

  if (file_handle == INVALID_HANDLE_VALUE){
    throw Errors::FileReadError(full_file_name, errno);
  }

  state = new ExtractionState;
  state->data_length = GetFileSize(file_handle, NULL);

  HANDLE file_mapping = CreateFileMapping(file_handle, NULL, PAGE_READONLY,
    0, 0, NULL);

  state->data = reinterpret_cast<uint8*>(MapViewOfFile(file_mapping,
    FILE_MAP_READ, 0, 0, 0));

  CloseHandle(file_handle);

// Anything else, just use plain fread
#else
  FILE* file = fopen(full_file_name.c_str(), "rb");

  if (!file){
    throw Errors::FileReadError(full_file_name, errno);
  }

  state = new ExtractionState;

  fseek(file, 0, SEEK_END);
  state->data_length = ftell(file);
  fseek(file, 0, SEEK_SET);

  state->data = new uint8[state->data_length];
  fread(state->data, 1, state->data_length, file);
  fclose(file);
#endif

  return state;
}

void Table::unload_data(ExtractionState* state) const throw () {
// On UNIX-based systems, remove the memory mapping to the file
#ifdef __unix
  munmap(state->data, state->data_length);

// ditto windows
#elif WIN32
  UnmapViewOfFile(state->data);

// some other OS, delete the allocated buffer
#else
  delete[] state->data;
#endif

  delete state;
}

} // namespace
