#include "xmlpatch.h"
#include <phnt_windows.h>
#include <phnt.h>
#include <ShlObj.h>
#include <KnownFolders.h>

#include <codecvt>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <string_view>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "versioninfo.h"
#include "binary_reader.h"
#include "fast_wild_compare.h"

#include <fmt/format.h>
#include <fnv1a.h>
#include <pe/module.h>
#include <pugixml.hpp>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include <xorstr.hpp>

std::wstring &replace_all(
  std::wstring &haystack,
  const std::wstring_view &search,
  const std::wstring_view &replace)
{
  auto searcher = std::boyer_moore_horspool_searcher(search.begin(), search.end());
  auto iterators = std::make_pair(haystack.begin(), haystack.begin());
  while ( iterators = searcher(iterators.first, haystack.end()), iterators.first != haystack.end() ) {
    auto pos = std::distance(haystack.begin(), iterators.first);
    haystack.replace(iterators.first, iterators.second, replace.begin(), replace.end());
    iterators.first = std::next(haystack.begin(), pos + replace.size());
  }
  return haystack;
}

const std::multimap<std::filesystem::path, std::vector<std::pair<std::wstring, std::wstring>>> get_or_load_addons()
{
  static auto once_flag = std::once_flag();
  static auto addons = std::multimap<std::filesystem::path, std::vector<std::pair<std::wstring, std::wstring>>>();

  std::call_once(once_flag, [](std::multimap<std::filesystem::path, std::vector<std::pair<std::wstring, std::wstring>>> &addons) {
    for ( const auto &entry : std::filesystem::directory_iterator(addons_path()) ) {
      if ( entry.is_regular_file()
        && fast_wild_compare(xorstr_(L"*.patch"), entry.path().filename()) ) {
        auto stream = std::wifstream(entry.path());
        stream.imbue(std::locale(stream.getloc(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::consume_header>));

        auto filename = std::filesystem::path();
        auto stext = std::vector<std::wstring>();
        auto rtext = std::vector<std::wstring>();
        auto line = std::wstring();
        while ( std::getline(stream, line) ) {
          auto ofs = line.find('=');
          if ( ofs == std::wstring::npos )
            continue;

          auto key = std::wstring_view(line.c_str(), ofs);
          if ( const auto it = std::find_if_not(key.begin(), key.end(), ::iswspace); it != key.end() )
            key.remove_prefix(std::distance(key.begin(), it));
          if ( const auto it = std::find_if_not(key.rbegin(), key.rend(), ::iswspace); it != key.rend() )
            key.remove_suffix(std::distance(key.rbegin(), it));

          auto value = std::wstring_view(&line[ofs + 1]);
          if ( const auto it = std::find_if_not(value.begin(), value.end(), ::iswspace); it != value.end() )
            value.remove_prefix(std::distance(value.begin(), it));
          if ( const auto it = std::find_if_not(value.rbegin(), value.rend(), ::iswspace); it != value.rend() )
            value.remove_suffix(std::distance(value.rbegin(), it));

          switch ( fnv1a::make_hash(key) ) {
            case L"FileName"_fnv1a: {
              filename = std::filesystem::weakly_canonical(value).filename();
              break;
            } case L"Search"_fnv1a: {
              auto s = static_cast<std::wstring>(value);
              stext.push_back(replace_all(s, xorstr_(L"NewLine"), xorstr_(L"\n")));
              break;
            } case L"Replace"_fnv1a: {
              auto s = static_cast<std::wstring>(value);
              rtext.push_back(replace_all(s, xorstr_(L"NewLine"), xorstr_(L"\n")));
              break;
            }
          }
        }

        if ( !filename.empty() && stext.size() == rtext.size() ) {
          auto vec = std::vector<std::pair<std::wstring, std::wstring>>();
          vec.reserve(stext.size());
          std::transform(stext.begin(), stext.end(), rtext.begin(), std::back_inserter(vec),
            [](std::wstring a, std::wstring b) { return std::make_pair(a, b);
          });
          addons.emplace(filename, vec);
        }
      }
    }
  }, addons);
  return addons;
}

const std::filesystem::path &addons_path()
{
  static auto once_flag = std::once_flag();
  static auto path = std::filesystem::path();

  std::call_once(once_flag, [](std::filesystem::path &path) {
    const wchar_t *OriginalFilename;
    if ( GetModuleVersionInfo(nullptr, xorstr_(L"\\StringFileInfo\\*\\OriginalFilename"), &(LPCVOID &)OriginalFilename) >= 0 ) {
      switch ( fnv1a::make_hash(OriginalFilename, towupper) ) {
        case L"Client.exe"_fnv1au:
          path = documents_path() / xorstr_(L"BnS\\addons");
          break;
        case L"BNSR.exe"_fnv1au:
          path = documents_path() / xorstr_(L"BNSR\\addons");
          break;
      }
    }
    std::filesystem::create_directories(path);
  }, path);
  return path;
}

pugi::xml_parse_result deserialize_document(const void *mem, const uint32_t size, pugi::xml_document &document)
{
  auto reader = binary_reader(mem, size);

  if ( mem && size > 0x50 && reader.geta<int64_t>() == 0x424C534F42584D4Ci64 ) { // rest of the header doesn't need to be size-checked
    const auto version = reader.geta<char>();
    if ( version != 3i8 ) {
      const auto text = fmt::format(xorstr_(L"Unknown binary XML version: {:#x}"), version);
      MessageBoxW(nullptr, text.c_str(), xorstr_(L"bnspatch"), MB_ICONERROR | MB_OK);
      exit(1);
    }

    const auto document_size = reader.geta<uint32_t>();
    if ( document_size != size ) {
      const auto text = fmt::format(xorstr_(L"Binary XML header size mismatch: {} != {}"), document_size, size);
      MessageBoxW(nullptr, text.c_str(), xorstr_(L"bnspatch"), MB_ICONERROR | MB_OK);
      exit(1);
    }

    reader.discard<char>(0x40);

    const auto valid = reader.get<bool>();
    const auto document_name = reader.get<std::wstring>();

    auto decl = document.prepend_child(pugi::node_declaration);
    decl.append_attribute(xorstr_(L"version")) = xorstr_(L"1.0");
    decl.append_attribute(xorstr_(L"encoding")) = xorstr_(L"utf-16");
    document.append_child(pugi::node_comment).set_value(document_name.c_str());
    deserialize_element(document, reader);
    auto res = pugi::xml_parse_result();
    res.status = pugi::status_ok;
    res.encoding = pugi::encoding_utf16_le;
    return res;
  } else {
    return document.load_buffer(mem, size, pugi::parse_full);
  }
  return pugi::xml_parse_result();
}

void deserialize_element(pugi::xml_node &parent, binary_reader &reader)
{
  auto node = parent.append_child();

  const auto attrcount = reader.get<uint32_t>();
  for ( uint32_t i = 0; i < attrcount; ++i ) {
    const auto attrname = reader.get<std::wstring>();
    const auto attrvalue = reader.get<std::wstring>();
    node.append_attribute(attrname.c_str()).set_value(attrvalue.c_str());
  }

  const auto valid = reader.get<bool>();
  const auto name = reader.get<std::wstring>();
  node.set_name(name.c_str());

  const auto children_count = reader.get<uint32_t>();
  const auto id = reader.get<uint32_t>();

  for ( auto i = 0ui32; i < children_count; ++i )
    deserialize_node(node, reader);
}

void deserialize_node(pugi::xml_node &parent, binary_reader &reader)
{
  const auto type = reader.get<uint32_t>();
  switch ( type ) {
    case 1: deserialize_element(parent, reader); break;
    case 2: deserialize_text(parent, reader); break;
    default:
      const auto text = fmt::format(xorstr_(L"Unknown node type: {:#x}"), type);
      MessageBoxW(nullptr, text.c_str(), xorstr_(L"bnspatch"), MB_ICONERROR | MB_OK);
      exit(1);
  }
}

void deserialize_text(pugi::xml_node &parent, binary_reader &reader)
{
  const auto pcdata = reader.get<std::wstring>();
  auto node = parent.append_child(pugi::node_pcdata);
  node.set_value(pcdata.c_str());
  reader.discard<bool>();
  reader.discard<uint16_t>(reader.get<uint32_t>()); // L"text"
  reader.discard<uint32_t>(); // child count
  reader.discard<uint32_t>(); // id
}

const std::filesystem::path &documents_path()
{
  static auto once_flag = std::once_flag();
  static auto path = std::filesystem::path();

  std::call_once(once_flag, [](std::filesystem::path &path) {
    auto result = wil::unique_cotaskmem_string();
    if ( SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &result)) )
      path = result.get();
  }, path);
  return path;
}

std::vector<std::pair<std::wstring, std::wstring>> get_relevant_addons(const wchar_t *xml)
{
  if ( !xml || !*xml )
    return {};

  auto relevant_addons = std::vector<std::pair<std::wstring, std::wstring>>();
  auto filename = std::filesystem::path(xml).filename();
  for ( const auto &addon : get_or_load_addons() ) {
    if ( !_wcsicmp(filename.c_str(), addon.first.c_str()) )
      relevant_addons.insert(relevant_addons.end(), addon.second.begin(), addon.second.end());
  }
  return relevant_addons;
}

std::vector<pugi::xml_node> get_relevant_patches(const wchar_t *xml)
{
  auto relevant_patches = std::vector<pugi::xml_node>();

  auto xml_path = std::filesystem::path(xml ? xml : L"");
  auto name = xorstr(L"patch");
  name.crypt();
  for ( const auto &patch : get_or_load_patches().document_element().children(name.get()) ) {
    auto attribute = patch.attribute(xorstr_(L"file"));
    if ( !attribute )
      attribute = patch.attribute(xorstr_(L"filename"));

    auto str = std::unique_ptr<wchar_t, decltype(&::free)>(::_wcsdup(attribute.value()), ::free);
    wchar_t *next_token;
    wchar_t *token = wcstok_s(str.get(), xorstr_(L";"), &next_token);
    while ( token ) {
      auto file = std::filesystem::path(token);
      if ( fast_wild_compare(file, xml_path)
        || fast_wild_compare(file, xml_path.filename()) )
        relevant_patches.push_back(patch);
      token = wcstok_s(nullptr, xorstr_(L";"), &next_token);
    }
  }
  return relevant_patches;
}

void patch_xml(pugi::xml_document &src, const std::vector<pugi::xml_node> &patches)
{
  for ( const auto &patch : patches ) {
    std::unordered_map<fnv1a::type, pugi::xml_node> saved_nodes;
    process_patch(src, patch.children(), saved_nodes);
  }
}

const pugi::xml_document &get_or_load_patches()
{
  static auto once_flag = std::once_flag();
  static auto document = pugi::xml_document();

  std::call_once(once_flag, [](pugi::xml_document &document) {

    auto decl = document.prepend_child(pugi::node_declaration);
    decl.append_attribute(xorstr_(L"version")) = xorstr_(L"1.0");
    decl.append_attribute(xorstr_(L"encoding")) = xorstr_(L"utf-8");

    document.append_child(xorstr_(L"patches"));

    auto include_guard = std::unordered_set<fnv1a::type>();
    preprocess(document, patches_path(), include_guard);
#ifdef _DEBUG
    document.save_file(patches_path().parent_path().append(xorstr_(L"preprocessed.xml")).c_str(), L"  ");
    const auto parent_path = patches_path().parent_path();
    std::filesystem::create_directories(parent_path / xorstr_(L"temp"));
    std::filesystem::create_directories(parent_path / xorstr_(L"temp_patched"));
#endif
  }, document);
  return document;
}

void preprocess(pugi::xml_document &patches_doc, const std::filesystem::path &path, std::unordered_set<fnv1a::type> &include_guard)
{
  pugi::xml_document document;
  if ( include_guard.emplace(fnv1a::make_hash(path.c_str(), towupper)).second
    && try_load_file(document, path, pugi::parse_default | pugi::parse_pi)
    && !_wcsicmp(document.document_element().name(), xorstr_(L"patches")) ) {

    for ( const auto &child : document.children() ) {
      switch ( child.type() ) {
        case pugi::node_element:
          if ( !_wcsicmp(child.name(), xorstr_(L"patches")) ) {
            auto xs_patch = xorstr(L"patch");
            xs_patch.crypt();

            patches_doc.document_element().append_child(pugi::node_comment).set_value(path.c_str());
            for ( const auto &patch : child.children(xs_patch.get()) ) {
              if ( patch.attribute(xorstr_(L"enabled")).as_bool(true) )
                patches_doc.document_element().append_copy(patch);
            }
          }
          break;

        case pugi::node_pi:
          if ( !_wcsicmp(child.name(), xorstr_(L"include")) ) {
            auto result = std::wstring();
            if ( FAILED(wil::ExpandEnvironmentStringsW(child.value(), result)) )
              continue;

            auto filter = std::filesystem::path(result);

            if ( filter.is_relative() )
              filter = path.parent_path() / filter;

            for ( const auto &it : std::filesystem::directory_iterator(filter.parent_path()) ) {
              if ( it.is_regular_file()
                && fast_wild_compare(filter.filename(), it.path().filename()) )
                preprocess(patches_doc, it.path(), include_guard);
            }
          }
          break;
      }
    }
  }
}

const std::filesystem::path &patches_path()
{
  static auto once_flag = std::once_flag();
  static auto path = std::filesystem::path();

  std::call_once(once_flag, [](std::filesystem::path &path) {
    auto result = wil::unique_cotaskmem_string();
    if ( SUCCEEDED(wil::TryGetEnvironmentVariableW(xorstr_(L"BNS_PROFILE_XML"), result)) && result ) {
      path = std::filesystem::canonical(result.get());
      return;
    }

    const wchar_t *OriginalFilename;
    if ( GetModuleVersionInfo(nullptr, xorstr_(L"\\StringFileInfo\\*\\OriginalFilename"), &(LPCVOID &)OriginalFilename) >= 0 ) {
      switch ( fnv1a::make_hash(OriginalFilename, towupper) ) {
        case L"Client.exe"_fnv1au:
          path = documents_path() / xorstr_(L"BnS\\patches.xml");
          return;
        case L"BNSR.exe"_fnv1au:
          path = documents_path() / xorstr_(L"BNSR\\patches.xml");
          return;
      }
    }

    path = std::filesystem::path(pe::get_module()->full_name()).parent_path().append(xorstr_(L"patches.xml"));
  }, path);
  return path;
}

void process_patch(
  const pugi::xpath_node &ctx,
  const pugi::xml_object_range<pugi::xml_node_iterator> &children,
  std::unordered_map<fnv1a::type, pugi::xml_node> &saved_nodes)
{
  for ( const auto &current : children ) {
    if ( ctx.attribute() ) {
      switch ( fnv1a::make_hash(current.name(), towupper) ) {
        case L"parent"_fnv1au: // ok
          process_patch(ctx.parent(), current.children(), saved_nodes);
          break;

        case L"set-name"_fnv1au: // ok
          ctx.attribute().set_name(current.attribute(xorstr_(L"name")).value());
          break;

        case L"set-value"_fnv1au: // ok
          ctx.attribute().set_value(current.attribute(xorstr_(L"value")).value());
          break;

        case L"previous-attribute"_fnv1au: // ok
          process_patch(pugi::xpath_node(ctx.attribute().previous_attribute(), ctx.parent()), current.children(), saved_nodes);
          break;

        case L"next-attribute"_fnv1au: // ok
          process_patch(pugi::xpath_node(ctx.attribute().next_attribute(), ctx.parent()), current.children(), saved_nodes);
          break;

        case L"insert-attribute-before"_fnv1au: { // ok
          auto attribute = ctx.node().insert_attribute_before(current.attribute(xorstr_(L"name")).value(), ctx.attribute());
          if ( const auto value = current.attribute(xorstr_(L"value")) )
            attribute.set_value(value.value());
          process_patch(pugi::xpath_node(attribute, ctx.node()), current.children(), saved_nodes);
          break;
        }
        case L"insert-attribute-after"_fnv1au: { // ok
          auto attribute = ctx.node().insert_attribute_after(current.attribute(xorstr_(L"name")).value(), ctx.attribute());
          if ( const auto value = current.attribute(xorstr_(L"value")) )
            attribute.set_value(value.value());
          process_patch(pugi::xpath_node(attribute, ctx.node()), current.children(), saved_nodes);
          break;
        }
        case L"remove"_fnv1au: // ok
          ctx.parent().remove_attribute(ctx.attribute());
          return;
      }
    } else {
      switch ( fnv1a::make_hash(current.name(), towupper) ) {
        case L"parent"_fnv1au: // ok
          process_patch(ctx.parent(), current.children(), saved_nodes);
          break;

        case L"select-node"_fnv1au: // ok
          process_patch(ctx.node().select_node(current.attribute(xorstr_(L"query")).value()), current.children(), saved_nodes);
          break;

        case L"select-nodes"_fnv1au: // ok
          for ( const auto &node : ctx.node().select_nodes(current.attribute(xorstr_(L"query")).value()) )
            process_patch(node, current.children(), saved_nodes);
          break;

        case L"prepend-attribute"_fnv1au: { // ok
          auto attribute = ctx.node().prepend_attribute(current.attribute(xorstr_(L"name")).value());
          if ( const auto value = current.attribute(xorstr_(L"value")) )
            attribute.set_value(value.value());
          process_patch(pugi::xpath_node(attribute, ctx.node()), current.children(), saved_nodes);
          break;
        }
        case L"append-attribute"_fnv1au: { // ok
          auto attribute = ctx.node().append_attribute(current.attribute(xorstr_(L"name")).value());
          if ( const auto value = current.attribute(xorstr_(L"value")) )
            attribute.set_value(value.value());
          process_patch(pugi::xpath_node(attribute, ctx.node()), current.children(), saved_nodes);
          break;
        }
        case L"prepend-child"_fnv1au: // ok
          process_patch(ctx.node().prepend_child(current.attribute(xorstr_(L"name")).value()), current.children(), saved_nodes);
          break;

        case L"append-buffer"_fnv1au: { // ok
          size_t size;
          const auto text = current.text();
          if ( SUCCEEDED(StringCbLengthW(text.get(), STRSAFE_MAX_CCH * sizeof(wchar_t), &size)) )
            ctx.node().append_buffer(text.get(), size, pugi::parse_default | pugi::parse_declaration | pugi::parse_fragment | pugi::parse_trim_pcdata, pugi::encoding_utf16);
          break;
        }
        case L"append-child"_fnv1au: // ok
          process_patch(ctx.node().append_child(current.attribute(xorstr_(L"name")).value()), current.children(), saved_nodes);
          break;

        case L"prepend-copy"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(ctx.node().prepend_copy(it->second), current.children(), saved_nodes);
          break;

        case L"append-copy"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(ctx.node().append_copy(it->second), current.children(), saved_nodes);
          break;

        case L"prepend-move"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(ctx.node().prepend_move(it->second), current.children(), saved_nodes);
          break;

        case L"append-move"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(ctx.node().append_move(it->second), current.children(), saved_nodes);
          break;

        case L"attribute"_fnv1au: // ok
          process_patch(pugi::xpath_node(ctx.node().attribute(current.attribute(xorstr_(L"name")).value()), ctx.node()), current.children(), saved_nodes);
          break;

        case L"attributes"_fnv1au: // ok
          for ( const auto &attr : ctx.node().attributes() )
            process_patch(pugi::xpath_node(attr, ctx.node()), current.children(), saved_nodes);
          break;

        case L"child"_fnv1au: // ok
          process_patch(ctx.node().child(current.attribute(xorstr_(L"name")).value()), current.children(), saved_nodes);
          break;

        case L"children"_fnv1au: // ok
          if ( const auto name = current.attribute(xorstr_(L"name")) ) {
            for ( const auto &child : ctx.node().children(name.value()) )
              process_patch(child, current.children(), saved_nodes);
          } else {
            for ( const auto &child : ctx.node().children() )
              process_patch(child, current.children(), saved_nodes);
          }
          break;

        case L"find-child-by-attribute"_fnv1au: // ok
          if ( const auto name = current.attribute(xorstr_(L"name")) ) {
            process_patch(ctx.node().find_child_by_attribute(
              name.value(),
              current.attribute(xorstr_(L"attribute-name")).value(),
              current.attribute(xorstr_(L"attribute-value")).value()),
              current.children(), saved_nodes);
          } else {
            process_patch(ctx.node().find_child_by_attribute(
              current.attribute(xorstr_(L"attribute-name")).value(),
              current.attribute(xorstr_(L"attribute-value")).value()),
              current.children(), saved_nodes);
          }
          break;

        case L"first-attribute"_fnv1au: // ok
          process_patch(pugi::xpath_node(ctx.node().first_attribute(), ctx.node()), current.children(), saved_nodes);
          break;

        case L"last-attribute"_fnv1au: // ok
          process_patch(pugi::xpath_node(ctx.node().last_attribute(), ctx.node()), current.children(), saved_nodes);
          break;

        case L"first-child"_fnv1au: // ok
          process_patch(ctx.node().first_child(), current.children(), saved_nodes);
          break;

        case L"last-child"_fnv1au: // ok
          process_patch(ctx.node().last_child(), current.children(), saved_nodes);
          break;

        case L"first-element-by-path"_fnv1au: // ok
          process_patch(ctx.node().first_element_by_path(current.attribute(xorstr_(L"path")).value()), current.children(), saved_nodes);
          break;

        case L"insert-sibling-after"_fnv1au: // ok
          process_patch(ctx.node().parent().insert_child_after(current.attribute(xorstr_(L"name")).value(), ctx.node()), current.children(), saved_nodes);
          break;

        case L"insert-sibling-before"_fnv1au: // ok
          process_patch(ctx.node().parent().insert_child_before(current.attribute(xorstr_(L"name")).value(), ctx.node()), current.children(), saved_nodes);
          break;

        case L"insert-copy-after"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(it->second.parent().insert_move_before(ctx.node(), it->second), current.children(), saved_nodes);
          break;

        case L"insert-copy-before"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(it->second.parent().insert_copy_before(ctx.node(), it->second), current.children(), saved_nodes);
          break;

        case L"insert-move-after"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(it->second.parent().insert_move_after(ctx.node(), it->second), current.children(), saved_nodes);
          break;

        case L"insert-move-before"_fnv1au: // ok
          if ( const auto it = saved_nodes.find(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper)); it != saved_nodes.end() )
            process_patch(it->second.parent().insert_move_before(ctx.node(), it->second), current.children(), saved_nodes);
          break;

        case L"next-sibling"_fnv1au: // ok
          process_patch(ctx.node().next_sibling(), current.children(), saved_nodes);
          break;

        case L"remove-attribute"_fnv1au: // ok
          ctx.node().remove_attribute(current.attribute(xorstr_(L"name")).value());
          break;

        case L"remove-attributes"_fnv1au: // ok
          ctx.node().remove_attributes();
          break;

        case L"remove-child"_fnv1au: // ok
          ctx.node().remove_child(current.attribute(xorstr_(L"name")).value());
          break;

        case L"remove-children"_fnv1au: // ok
          ctx.node().remove_children();
          break;

        case L"root"_fnv1au: // ok
          process_patch(ctx.node().root(), current.children(), saved_nodes);
          break;

        case L"set-name"_fnv1au: // ok
          ctx.node().set_name(current.attribute(xorstr_(L"name")).value());
          break;

        case L"set-value"_fnv1au: // ok
          ctx.node().set_value(current.attribute(xorstr_(L"value")).value());
          break;

        case L"assign-node-key"_fnv1au: // ok
          saved_nodes.insert_or_assign(fnv1a::make_hash(current.attribute(xorstr_(L"node-key")).value(), towupper), ctx.node());
          break;

        case L"remove"_fnv1au: // ok
          ctx.node().parent().remove_child(ctx.node());
          return;
      }
    }
  }
}

pugi::xml_parse_result try_load_file(
  pugi::xml_document &document,
  const std::filesystem::path &path,
  unsigned int options,
  pugi::xml_encoding encoding)
{
try_again:
  const auto result = document.load_file(path.c_str(), options, encoding);
  if ( !result && result.status != pugi::xml_parse_status::status_file_not_found ) {
    switch ( MessageBoxA(
      nullptr,
      fmt::format(xorstr_("{}({}): {}"), path.string(), result.offset, result.description()).c_str(),
      xorstr_("bnspatch"),
      MB_CANCELTRYCONTINUE | MB_ICONERROR) ) {
      case IDTRYAGAIN: goto try_again;
      case IDCONTINUE: break;
      default: exit(0);
    }
  }
  return result;
}
