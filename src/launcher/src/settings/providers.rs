//! Run-time option lists for rows that name a `provider` (dist RL13).
//!
//! The only one today is `lang_packs`: the Language row offers English (stored as the empty value,
//! which is what the game's `[lang] pack=` means for "no pack") plus every pack the player has built
//! under `<game>\lang\<id>\`. A directory counts as a pack when it carries `pack.ini` (RL17 writes
//! one per pack, with the codepage) or the pack's `Msgs.dat`. Packs are listed by their own
//! language name, never translated: a Russian player must be able to find "Русский" in an English UI.

use std::path::Path;

use super::model::ProviderOption;

/// A language's name in that language, for the ids the project builds packs for. Unknown ids show
/// as the id itself.
pub fn language_autonym(id: &str) -> Option<&'static str> {
    Some(match id.to_ascii_lowercase().as_str() {
        "en" => "English",
        "ru" => "Русский",
        "de" => "Deutsch",
        "fr" => "Français",
        "it" => "Italiano",
        "pl" => "Polski",
        _ => return None,
    })
}

/// Does this directory look like a built language pack?
fn is_pack_dir(dir: &Path) -> bool {
    dir.join("pack.ini").is_file() || dir.join("Msgs.dat").is_file()
}

/// `""` (English) first, then the packs under `<game_dir>\lang\`, sorted by id. A missing game
/// directory or `lang\` folder is just "English only".
pub fn lang_packs(game_dir: Option<&Path>) -> Vec<ProviderOption> {
    let mut out = vec![ProviderOption {
        value: String::new(),
        label: "English".to_string(),
    }];
    let Some(game_dir) = game_dir else { return out };
    let Ok(rd) = std::fs::read_dir(game_dir.join("lang")) else {
        return out;
    };
    let mut ids: Vec<String> = rd
        .filter_map(Result::ok)
        .filter(|e| e.path().is_dir() && is_pack_dir(&e.path()))
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    ids.sort_unstable_by_key(|s| s.to_ascii_lowercase());
    for id in ids {
        // "en" would be a second spelling of the stock language; the empty value already says it.
        if id.eq_ignore_ascii_case("en") {
            continue;
        }
        let label = match language_autonym(&id) {
            Some(n) => n.to_string(),
            None => id.clone(),
        };
        out.push(ProviderOption { value: id, label });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn english_is_always_offered_and_packs_are_found_by_their_files() {
        assert_eq!(lang_packs(None).len(), 1);
        let dir = std::env::temp_dir().join("mh_launcher_test_providers");
        let _ = std::fs::remove_dir_all(&dir);
        let lang = dir.join("lang");
        for (id, file) in [
            ("ru", "pack.ini"),
            ("pl", "Msgs.dat"),
            ("empty", "readme.txt"),
            ("en", "pack.ini"),
        ] {
            std::fs::create_dir_all(lang.join(id)).unwrap();
            std::fs::write(lang.join(id).join(file), "x").unwrap();
        }
        // A stray FILE named like a pack is not one.
        std::fs::write(lang.join("de"), "x").unwrap();
        let got = lang_packs(Some(&dir));
        let pairs: Vec<(&str, &str)> = got
            .iter()
            .map(|o| (o.value.as_str(), o.label.as_str()))
            .collect();
        assert_eq!(
            pairs,
            [("", "English"), ("pl", "Polski"), ("ru", "Русский")]
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_game_dir_without_lang_is_english_only() {
        let dir = std::env::temp_dir().join("mh_launcher_test_providers_none");
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(lang_packs(Some(&dir)).len(), 1);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
