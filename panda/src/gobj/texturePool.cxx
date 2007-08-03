// Filename: texturePool.cxx
// Created by:  drose (26Apr00)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001 - 2004, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://etc.cmu.edu/panda3d/docs/license/ .
//
// To contact the maintainers of this program write to
// panda3d-general@lists.sourceforge.net .
//
////////////////////////////////////////////////////////////////////

#include "texturePool.h"
#include "config_gobj.h"
#include "config_util.h"
#include "config_express.h"
#include "string_utils.h"
#include "virtualFileSystem.h"
#include "bamCache.h"
#include "bamCacheRecord.h"
#include "pnmFileTypeRegistry.h"
#include "texturePoolFilter.h"
#include "configVariableList.h"
#include "load_dso.h"
#include "mutexHolder.h"

TexturePool *TexturePool::_global_ptr;

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::write
//       Access: Published, Static
//  Description: Lists the contents of the texture pool to the
//               indicated output stream.
//               For debugging.
////////////////////////////////////////////////////////////////////
void TexturePool::
write(ostream &out) {
  get_global_ptr()->ns_list_contents(out);
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::register_texture_type
//       Access: Public
//  Description: Records a factory function that makes a Texture
//               object of the appropriate type for one or more
//               particular filename extensions.  The string
//               extensions may be a string that contains
//               space-separated list of extensions, case-insensitive.
////////////////////////////////////////////////////////////////////
void TexturePool::
register_texture_type(MakeTextureFunc *func, const string &extensions) {
  MutexHolder holder(_lock);

  vector_string words;
  extract_words(downcase(extensions), words);

  vector_string::const_iterator wi;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    _type_registry[*wi] = func;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::register_filter
//       Access: Public
//  Description: Records a TexturePoolFilter object that may operate
//               on texture images as they are loaded from disk.
////////////////////////////////////////////////////////////////////
void TexturePool::
register_filter(TexturePoolFilter *filter) {
  MutexHolder holder(_lock);

  gobj_cat.info()
    << "Registering Texture filter " << *filter << "\n";
  _filter_registry.push_back(filter);
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::get_texture_type
//       Access: Public
//  Description: Returns the factory function to construct a new
//               texture of the type appropriate for the indicated
//               filename extension, if any, or NULL if the extension
//               is not one of the extensions for a texture file.
////////////////////////////////////////////////////////////////////
TexturePool::MakeTextureFunc *TexturePool::
get_texture_type(const string &extension) const {
  MutexHolder holder(_lock);

  string c = downcase(extension);
  TypeRegistry::const_iterator ti;
  ti = _type_registry.find(extension);
  if (ti != _type_registry.end()) {
    return (*ti).second;
  }

  // Check the PNM type registry.
  PNMFileTypeRegistry *pnm_reg = PNMFileTypeRegistry::get_global_ptr();
  PNMFileType *type = pnm_reg->get_type_from_extension(extension);
  if (type != (PNMFileType *)NULL) {
    // This is a known image type; create an ordinary Texture.
    ((TexturePool *)this)->_type_registry[extension] = Texture::make_texture;
    return Texture::make_texture;
  }

  // This is an unknown texture type.
  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::make_texture
//       Access: Public
//  Description: Creates a new Texture object of the appropriate type
//               for the indicated filename extension, according to
//               the types that have been registered via
//               register_texture_type().
////////////////////////////////////////////////////////////////////
PT(Texture) TexturePool::
make_texture(const string &extension) const {
  MakeTextureFunc *func = get_texture_type(extension);
  if (func != NULL) {
    return func();
  }

  // We don't know what kind of file type this is; return an ordinary
  // Texture in case it's an image file with no extension.
  return new Texture;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::write_texture_types
//       Access: Public
//  Description: Outputs a list of the available texture types to the
//               indicated output stream.  This is mostly the list of
//               available image types, with maybe a few additional
//               ones for video textures.
////////////////////////////////////////////////////////////////////
void TexturePool::
write_texture_types(ostream &out, int indent_level) const {
  MutexHolder holder(_lock);

  PNMFileTypeRegistry *pnm_reg = PNMFileTypeRegistry::get_global_ptr();
  pnm_reg->write(out, indent_level);

  // Also output any of the additional texture types, that aren't
  // strictly images (these are typically video textures).
  TypeRegistry::const_iterator ti;
  for (ti = _type_registry.begin(); ti != _type_registry.end(); ++ti) {
    string extension = (*ti).first;
    MakeTextureFunc *func = (*ti).second;

    if (pnm_reg->get_type_from_extension(extension) == NULL) {
      PT(Texture) tex = func();
      string name = tex->get_type().get_name();
      indent(out, indent_level) << name;
      indent(out, max(30 - (int)name.length(), 0))
        << "  ." << extension << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::get_global_ptr
//       Access: Public, Static
//  Description: Initializes and/or returns the global pointer to the
//               one TexturePool object in the system.
////////////////////////////////////////////////////////////////////
TexturePool *TexturePool::
get_global_ptr() {
  if (_global_ptr == (TexturePool *)NULL) {
    _global_ptr = new TexturePool;

    // We have to call this here, not in the constructor, so that the
    // _global_ptr is safely assigned by the time the filters begin to
    // load.
    _global_ptr->load_filters();
  }
  return _global_ptr;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::Constructor
//       Access: Private
//  Description: The constructor is not intended to be called
//               directly; there's only supposed to be one TexturePool
//               in the universe and it constructs itself.
////////////////////////////////////////////////////////////////////
TexturePool::
TexturePool() {
  ConfigVariableString fake_texture_image
    ("fake-texture-image", "",
     PRC_DESC("Set this to enable a speedy-load mode in which you don't care "
              "what the world looks like, you just want it to load in minimal "
              "time.  This causes all texture loads via the TexturePool to use "
              "the same texture file, which will presumably only be loaded "
              "once."));
  _fake_texture_image = fake_texture_image;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_has_texture
//       Access: Private
//  Description: The nonstatic implementation of has_texture().
////////////////////////////////////////////////////////////////////
bool TexturePool::
ns_has_texture(const Filename &orig_filename) {
  MutexHolder holder(_lock);

  Filename filename(orig_filename);

  if (!_fake_texture_image.empty()) {
    filename = _fake_texture_image;
  }

  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  vfs->resolve_filename(filename, get_texture_path());
  vfs->resolve_filename(filename, get_model_path());

  Textures::const_iterator ti;
  ti = _textures.find(filename);
  if (ti != _textures.end()) {
    // This texture was previously loaded.
    return true;
  }

  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_load_texture
//       Access: Private
//  Description: The nonstatic implementation of load_texture().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_load_texture(const Filename &orig_filename, int primary_file_num_channels,
    bool read_mipmaps) {
  Filename filename(orig_filename);

  if (!_fake_texture_image.empty()) {
    filename = _fake_texture_image;
  }

  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  vfs->resolve_filename(filename, get_texture_path()) ||
    vfs->resolve_filename(filename, get_model_path());

  {
    MutexHolder holder(_lock);
    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      Texture *tex = (*ti).second;
      nassertr(!tex->get_fullpath().empty(), tex);
      return tex;
    }
  }

  // The texture was not found in the pool.
  PT(Texture) tex;
  PT(BamCacheRecord) record;
  bool store_record = false;

  // Can one of our texture filters supply the texture?
  tex = pre_load(orig_filename, Filename(), primary_file_num_channels, 0,
                 read_mipmaps);

  BamCache *cache = BamCache::get_global_ptr();
  if (tex == (Texture *)NULL) {
    // The texture was not supplied by a texture filter.  See if it
    // can be found in the on-disk cache, if it is active.
    if (cache->get_active() && !textures_header_only) {
      record = cache->lookup(filename, "txo");
      if (record != (BamCacheRecord *)NULL) {
        if (record->has_data()) {
          gobj_cat.info()
            << "Texture " << filename << " found in disk cache.\n";
          tex = DCAST(Texture, record->extract_data());
          if (!preload_textures) {
            // But drop the RAM until we need it.
            tex->clear_ram_image();
          }
          tex->set_keep_ram_image(false);
        }
      }
    }
  }

  if (tex == (Texture *)NULL) {
    // The texture was neither in the pool, nor found in the on-disk
    // cache; it needs to be loaded from its source image(s).
    gobj_cat.info()
      << "Loading texture " << filename << "\n";
    tex = make_texture(filename.get_extension());
    if (!tex->read(filename, Filename(), primary_file_num_channels, 0,
                   0, 0, false, read_mipmaps, record)) {
      // This texture was not found or could not be read.
      report_texture_unreadable(filename);
      return NULL;
    }
    store_record = (record != (BamCacheRecord *)NULL);
  }

  // Set the original filename, before we searched along the path.
  nassertr(tex != (Texture *)NULL, NULL);
  tex->set_filename(orig_filename);
  tex->set_fullpath(filename);
  tex->_texture_pool_key = filename;

  {
    MutexHolder holder(_lock);

    // Now look again--someone may have just loaded this texture in
    // another thread.
    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      Texture *tex = (*ti).second;
      nassertr(!tex->get_fullpath().empty(), tex);
      return tex;
    }

    _textures[filename] = tex;
  }

  if (store_record) {
    // Store the on-disk cache record for next time.
    nassertr(tex->has_ram_image(), tex);
    record->set_data(tex, false);
    cache->store(record);

    if (!preload_textures) {
      // And now drop the RAM until we need it.
      tex->clear_ram_image();
    }
  }

  nassertr(!tex->get_fullpath().empty(), tex);

  // Finally, apply any post-loading texture filters.
  tex = post_load(tex);

  return tex;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_load_texture
//       Access: Private
//  Description: The nonstatic implementation of load_texture().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_load_texture(const Filename &orig_filename, 
                const Filename &orig_alpha_filename,
                int primary_file_num_channels,
                int alpha_file_channel,
    bool read_mipmaps) {
  Filename filename(orig_filename);
  Filename alpha_filename(orig_alpha_filename);

  if (!_fake_texture_image.empty()) {
    return ns_load_texture(_fake_texture_image, primary_file_num_channels,
         read_mipmaps);
  }

  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  vfs->resolve_filename(filename, get_texture_path()) ||
    vfs->resolve_filename(filename, get_model_path());
  
  vfs->resolve_filename(alpha_filename, get_texture_path()) ||
    vfs->resolve_filename(alpha_filename, get_model_path());

  {
    MutexHolder holder(_lock);

    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      Texture *tex = (*ti).second;
      nassertr(!tex->get_fullpath().empty(), tex);
      return tex;
    }
  }

  PT(Texture) tex;
  PT(BamCacheRecord) record;
  bool store_record = false;

  // Can one of our texture filters supply the texture?
  tex = pre_load(orig_filename, alpha_filename, primary_file_num_channels, 
                 alpha_file_channel, read_mipmaps);

  BamCache *cache = BamCache::get_global_ptr();
  if (tex == (Texture *)NULL) {
    // The texture was not supplied by a texture filter.  See if it
    // can be found in the on-disk cache, if it is active.
    if (cache->get_active() && !textures_header_only) {
      // See if the texture can be found in the on-disk cache, if it is
      // active.
      record = cache->lookup(filename, "txo");
      if (record != (BamCacheRecord *)NULL) {
        if (record->has_data()) {
          gobj_cat.info()
            << "Texture " << filename << " found in disk cache.\n";
          tex = DCAST(Texture, record->extract_data());
        }
      }
    }
  }

  if (tex == (Texture *)NULL) {
    // The texture was neither in the pool, nor found in the on-disk
    // cache; it needs to be loaded from its source image(s).
    gobj_cat.info()
      << "Loading texture " << filename << " and alpha component "
      << alpha_filename << endl;
    tex = make_texture(filename.get_extension());
    if (!tex->read(filename, alpha_filename, primary_file_num_channels,
                   alpha_file_channel, 0, 0, false, read_mipmaps)) {
      // This texture was not found or could not be read.
      report_texture_unreadable(filename);
      return NULL;
    }
    store_record = (record != (BamCacheRecord *)NULL);
  }

  // Set the original filenames, before we searched along the path.
  nassertr(tex != (Texture *)NULL, NULL);
  tex->set_filename(orig_filename);
  tex->set_fullpath(filename);
  tex->set_alpha_filename(orig_alpha_filename);
  tex->set_alpha_fullpath(alpha_filename);
  tex->_texture_pool_key = filename;

  {
    MutexHolder holder(_lock);

    // Now look again.
    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      Texture *tex = (*ti).second;
      nassertr(!tex->get_fullpath().empty(), tex);
      return tex;
    }
    
    _textures[filename] = tex;
  }

  if (store_record) {
    // Store the on-disk cache record for next time.
    record->set_data(tex, false);
    cache->store(record);
  }

  nassertr(!tex->get_fullpath().empty(), tex);

  // Finally, apply any post-loading texture filters.
  tex = post_load(tex);

  return tex;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_load_3d_texture
//       Access: Private
//  Description: The nonstatic implementation of load_3d_texture().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_load_3d_texture(const Filename &filename_pattern,
       bool read_mipmaps) {
  Filename filename(filename_pattern);
  filename.set_pattern(true);

  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  vfs->resolve_filename(filename, get_texture_path()) ||
    vfs->resolve_filename(filename, get_model_path());

  {
    MutexHolder holder(_lock);

    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      return (*ti).second;
    }
  }

  PT(Texture) tex;
  PT(BamCacheRecord) record;
  bool store_record = false;

  BamCache *cache = BamCache::get_global_ptr();
  if (cache->get_active() && !textures_header_only) {
    // See if the texture can be found in the on-disk cache, if it is
    // active.
    record = cache->lookup(filename, "txo");
    if (record != (BamCacheRecord *)NULL) {
      if (record->has_data()) {
        gobj_cat.info()
          << "Texture " << filename << " found in disk cache.\n";
        tex = DCAST(Texture, record->extract_data());
      }
    }
  }

  if (tex == (Texture *)NULL || 
      tex->get_texture_type() != Texture::TT_3d_texture) {
    // The texture was neither in the pool, nor found in the on-disk
    // cache; it needs to be loaded from its source image(s).
    gobj_cat.info()
      << "Loading 3-d texture " << filename << "\n";
    tex = make_texture(filename.get_extension());
    tex->setup_3d_texture();
    if (!tex->read(filename, 0, 0, true, read_mipmaps)) {
      // This texture was not found or could not be read.
      report_texture_unreadable(filename);
      return NULL;
    }
    store_record = (record != (BamCacheRecord *)NULL);
  }

  // Set the original filename, before we searched along the path.
  nassertr(tex != (Texture *)NULL, false);
  tex->set_filename(filename_pattern);
  tex->set_fullpath(filename);
  tex->_texture_pool_key = filename;

  {
    MutexHolder holder(_lock);

    // Now look again.
    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      return (*ti).second;
    }

    _textures[filename] = tex;
  }

  if (store_record) {
    // Store the on-disk cache record for next time.
    record->set_data(tex, false);
    cache->store(record);
  }

  nassertr(!tex->get_fullpath().empty(), tex);
  return tex;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_load_cube_map
//       Access: Private
//  Description: The nonstatic implementation of load_cube_map().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_load_cube_map(const Filename &filename_pattern, bool read_mipmaps) {
  Filename filename(filename_pattern);
  filename.set_pattern(true);

  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  vfs->resolve_filename(filename, get_texture_path()) ||
    vfs->resolve_filename(filename, get_model_path());

  {
    MutexHolder holder(_lock);

    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      return (*ti).second;
    }
  }

  PT(Texture) tex;
  PT(BamCacheRecord) record;
  bool store_record = false;

  BamCache *cache = BamCache::get_global_ptr();
  if (cache->get_active() && !textures_header_only) {
    // See if the texture can be found in the on-disk cache, if it is
    // active.
    record = cache->lookup(filename, "txo");
    if (record != (BamCacheRecord *)NULL) {
      if (record->has_data()) {
        gobj_cat.info()
          << "Texture " << filename << " found in disk cache.\n";
        tex = DCAST(Texture, record->extract_data());
      }
    }
  }

  if (tex == (Texture *)NULL || 
      tex->get_texture_type() != Texture::TT_cube_map) {
    // The texture was neither in the pool, nor found in the on-disk
    // cache; it needs to be loaded from its source image(s).
    gobj_cat.info()
      << "Loading cube map texture " << filename << "\n";
    tex = make_texture(filename.get_extension());
    tex->setup_cube_map();
    if (!tex->read(filename, 0, 0, true, read_mipmaps)) {
      // This texture was not found or could not be read.
      report_texture_unreadable(filename);
      return NULL;
    }
    store_record = (record != (BamCacheRecord *)NULL);
  }
    
  // Set the original filename, before we searched along the path.
  nassertr(tex != (Texture *)NULL, false);
  tex->set_filename(filename_pattern);
  tex->set_fullpath(filename);
  tex->_texture_pool_key = filename;

  {
    MutexHolder holder(_lock);

    // Now look again.
    Textures::const_iterator ti;
    ti = _textures.find(filename);
    if (ti != _textures.end()) {
      // This texture was previously loaded.
      return (*ti).second;
    }

    _textures[filename] = tex;
  }

  if (store_record) {
    // Store the on-disk cache record for next time.
    record->set_data(tex, false);
    cache->store(record);
  }

  nassertr(!tex->get_fullpath().empty(), tex);
  return tex;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_get_normalization_cube_map
//       Access: Private
//  Description: The nonstatic implementation of get_normalization_cube_map().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_get_normalization_cube_map(int size) {
  MutexHolder holder(_lock);

  if (_normalization_cube_map == (Texture *)NULL) {
    _normalization_cube_map = new Texture("normalization_cube_map");
  }
  if (_normalization_cube_map->get_x_size() < size ||
      _normalization_cube_map->get_texture_type() != Texture::TT_cube_map) {
    _normalization_cube_map->generate_normalization_cube_map(size);
  }

  return _normalization_cube_map;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_get_alpha_scale_map
//       Access: Private
//  Description: The nonstatic implementation of get_alpha_scale_map().
////////////////////////////////////////////////////////////////////
Texture *TexturePool::
ns_get_alpha_scale_map() {
  MutexHolder holder(_lock);

  if (_alpha_scale_map == (Texture *)NULL) {
    _alpha_scale_map = new Texture("alpha_scale_map");
    _alpha_scale_map->generate_alpha_scale_map();
  }

  return _alpha_scale_map;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_add_texture
//       Access: Private
//  Description: The nonstatic implementation of add_texture().
////////////////////////////////////////////////////////////////////
void TexturePool::
ns_add_texture(Texture *tex) {
  PT(Texture) keep = tex;
  MutexHolder holder(_lock);

  if (!tex->_texture_pool_key.empty()) {
    ns_release_texture(tex);
  }
  string filename = tex->get_fullpath();
  if (filename.empty()) {
    gobj_cat.error() << "Attempt to call add_texture() on an unnamed texture.\n";
  }

  // We blow away whatever texture was there previously, if any.
  tex->_texture_pool_key = filename;
  _textures[filename] = tex;
  nassertv(!tex->get_fullpath().empty());
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_release_texture
//       Access: Private
//  Description: The nonstatic implementation of release_texture().
////////////////////////////////////////////////////////////////////
void TexturePool::
ns_release_texture(Texture *tex) {
  MutexHolder holder(_lock);

  if (!tex->_texture_pool_key.empty()) {
    Textures::iterator ti;
    ti = _textures.find(tex->_texture_pool_key);
    if (ti != _textures.end() && (*ti).second == tex) {
      _textures.erase(ti);
    }
    tex->_texture_pool_key = string();
  }
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_release_all_textures
//       Access: Private
//  Description: The nonstatic implementation of release_all_textures().
////////////////////////////////////////////////////////////////////
void TexturePool::
ns_release_all_textures() {
  MutexHolder holder(_lock);

  Textures::iterator ti;
  for (ti = _textures.begin(); ti != _textures.end(); ++ti) {
    Texture *tex = (*ti).second;
    tex->_texture_pool_key = string();
  }

  _textures.clear();
  _normalization_cube_map = NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_garbage_collect
//       Access: Private
//  Description: The nonstatic implementation of garbage_collect().
////////////////////////////////////////////////////////////////////
int TexturePool::
ns_garbage_collect() {
  MutexHolder holder(_lock);

  int num_released = 0;
  Textures new_set;

  Textures::iterator ti;
  for (ti = _textures.begin(); ti != _textures.end(); ++ti) {
    Texture *tex = (*ti).second;
    if (tex->get_ref_count() == 1) {
      if (gobj_cat.is_debug()) {
        gobj_cat.debug()
          << "Releasing " << (*ti).first << "\n";
      }
      ++num_released;
      tex->_texture_pool_key = string();
    } else {
      new_set.insert(new_set.end(), *ti);
    }
  }

  _textures.swap(new_set);

  if (_normalization_cube_map != (Texture *)NULL &&
      _normalization_cube_map->get_ref_count() == 1) {
    if (gobj_cat.is_debug()) {
      gobj_cat.debug()
        << "Releasing normalization cube map\n";
    }
    ++num_released;
    _normalization_cube_map = NULL;
  }

  return num_released;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::ns_list_contents
//       Access: Private
//  Description: The nonstatic implementation of list_contents().
////////////////////////////////////////////////////////////////////
void TexturePool::
ns_list_contents(ostream &out) const {
  MutexHolder holder(_lock);

  int total_size;
  int total_ram_size;
  Textures::const_iterator ti;

  out << "texture pool contents:\n";
  
  total_size = 0;
  total_ram_size = 0;
  for (ti = _textures.begin(); ti != _textures.end(); ++ti) {
    Texture *tex = (*ti).second;
    out << (*ti).first << "\n";
    out << "  (count = " << tex->get_ref_count() 
        << ", ram  = " << tex->get_ram_image_size() 
        << ", size = " << tex->get_ram_page_size()
        << ", w = " << tex->get_x_size() 
        << ", h = " << tex->get_y_size() 
        << ")\n";
    nassertv(tex->_texture_pool_key == (*ti).first);
    total_ram_size += tex->get_ram_image_size();
    total_size += tex->get_ram_page_size();
  }
  
  out << "total number of textures: " << _textures.size() << "\n";
  out << "texture pool ram : " << total_ram_size << "\n";
  out << "texture pool size: " << total_size << "\n";
  out << "texture pool size - texture pool ram: " << total_size - total_ram_size << "\n";
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::report_texture_unreadable
//       Access: Private
//  Description: Prints a suitable error message when a texture could
//               not be loaded.
////////////////////////////////////////////////////////////////////
void TexturePool::
report_texture_unreadable(const Filename &filename) const {
  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  if (!vfs->exists(filename)) {
    if (filename.is_local()) {
      // The file doesn't exist, and it wasn't
      // fully-qualified--therefore, it wasn't found along either
      // search path.
      gobj_cat.error()
        << "Unable to find texture \"" << filename << "\""
        << " on texture_path " << texture_path
        << " or model_path " << model_path <<"\n";
    } else {
      // A fully-specified filename is not searched along the path, so
      // don't mislead the user with the error message.
      gobj_cat.error()
        << "Texture \"" << filename << "\" does not exist.\n";
    }

  } else {
    // The file exists, but it couldn't be read for some reason.
    gobj_cat.error()
      << "Texture \"" << filename << "\" exists but cannot be read.\n";

    // Maybe the filename extension is unknown.
    MakeTextureFunc *func = get_texture_type(filename.get_extension());
    if (func == (MakeTextureFunc *)NULL) {
      gobj_cat.error()
        << "Texture extension \"" << filename.get_extension() 
        << "\" is unknown.  Supported texture types:\n";
      write_texture_types(gobj_cat.error(false), 2);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::pre_load
//       Access: Private
//  Description: Invokes pre_load() on all registered filters until
//               one returns non-NULL; returns NULL if there are no
//               registered filters or if all registered filters
//               returned NULL.
////////////////////////////////////////////////////////////////////
PT(Texture) TexturePool::
pre_load(const Filename &orig_filename, const Filename &orig_alpha_filename,
         int primary_file_num_channels, int alpha_file_channel,
         bool read_mipmaps) {
  PT(Texture) tex;

  MutexHolder holder(_lock);

  FilterRegistry::iterator fi;
  for (fi = _filter_registry.begin();
       fi != _filter_registry.end();
       ++fi) {
    tex = (*fi)->pre_load(orig_filename, orig_alpha_filename,
                          primary_file_num_channels, alpha_file_channel,
                          read_mipmaps);
    if (tex != (Texture *)NULL) {
      return tex;
    }
  }

  return tex;
}

////////////////////////////////////////////////////////////////////
//     Function: TexturePool::post_load
//       Access: Public, Virtual
//  Description: Invokes post_load() on all registered filters.
////////////////////////////////////////////////////////////////////
PT(Texture) TexturePool::
post_load(Texture *tex) {
  PT(Texture) result = tex;

  MutexHolder holder(_lock);

  FilterRegistry::iterator fi;
  for (fi = _filter_registry.begin();
       fi != _filter_registry.end();
       ++fi) {
    result = (*fi)->post_load(result);
  }

  return result;
}


////////////////////////////////////////////////////////////////////
//     Function: TexturePool::load_filters
//       Access: Private
//  Description: Loads up all of the dll's named by the texture-filter
//               Config.prc variable.
////////////////////////////////////////////////////////////////////
void TexturePool::
load_filters() {
  ConfigVariableList texture_filter
    ("texture-filter",
     PRC_DESC("Names one or more external libraries that should be loaded for the "
              "purposes of performing texture filtering.  This variable may be repeated several "
              "times.  As in load-display, the actual library filename is derived by "
              "prefixing 'lib' to the specified name."));
  
  int num_aux = texture_filter.get_num_unique_values();
  for (int i = 0; i < num_aux; i++) {
    string name = texture_filter.get_unique_value(i);
    
    Filename dlname = Filename::dso_filename("lib" + name + ".so");
    gobj_cat->info()
      << "loading texture filter: " << dlname.to_os_specific() << endl;
    void *tmp = load_dso(dlname);
    if (tmp == (void *)NULL) {
      gobj_cat.info()
        << "Unable to load: " << load_dso_error() << endl;
    }
  }
}
