"""Wiki art extraction: icons, building/unit sprites, planet frames, video stills, review sheets.

Every ``extract_*`` returns ``{asset_key: path relative to out_dir}``; counters and problems are
collected in ``ArtSource.stats`` / ``ArtSource.notes``.
"""

from .common import DEFAULT_CACHE, ArtSource
from .icons import decode_gfx, extract_icons, read_icon_table, used_icon_names
from .planetmaps import extract_planet_maps
from .planets import extract_planets
from .sheets import building_sheet, unit_sheets
from .sprites import (
    SpriteStore,
    extract_building_sprites,
    extract_unit_sprites,
    load_facing,
    object_keys,
)
from .stills import extract_stills, stills_review_sheets, video_refs

__all__ = [
    "DEFAULT_CACHE",
    "ArtSource",
    "SpriteStore",
    "building_sheet",
    "decode_gfx",
    "extract_building_sprites",
    "extract_icons",
    "extract_planet_maps",
    "extract_planets",
    "extract_stills",
    "extract_unit_sprites",
    "load_facing",
    "object_keys",
    "read_icon_table",
    "stills_review_sheets",
    "unit_sheets",
    "used_icon_names",
    "video_refs",
]
