"""epdbook — ESP32-S3 极简 .ebook 格式与转换 / 调试阅读。"""

__version__ = "1.0.0"

from .format import (
    MAGIC,
    VERSION,
    FLAG_HAS_COVER,
    FLAG_IMG_A2I1,
    FLAG_PAGE_IMAGES,
    FLAG_ZLIB,
    BLOCK_TEXT,
    BLOCK_IMAGE,
    IMG_JPEG,
    IMG_PNG,
    IMG_A2I1,
    IMG_GRAY,
    CTRL_PARA,
    HEADER_SIZE,
    CHAPTER_ENTRY_SIZE,
    BLOCK_HEADER_SIZE,
)
from .writer import EbookWriter, ChapterDraft, ContentItem
from .reader import (
    ContentPiece,
    EbookFile,
    decode_image_to_pil,
    load_chapter,
    load_cover_payload,
    open_ebook,
)
from .cover_ops import (
    CoverInfo,
    CoverMutateResult,
    clear_ebook_cover,
    cover_info,
    encode_cover_image,
    set_ebook_cover,
)
from .meta_extra import (
    book_id_from_extra,
    generate_book_id,
    normalize_book_id,
    resolve_book_id,
)

__all__ = [
    "MAGIC",
    "VERSION",
    "FLAG_HAS_COVER",
    "FLAG_IMG_A2I1",
    "FLAG_PAGE_IMAGES",
    "FLAG_ZLIB",
    "BLOCK_TEXT",
    "BLOCK_IMAGE",
    "IMG_JPEG",
    "IMG_PNG",
    "IMG_A2I1",
    "IMG_GRAY",
    "CTRL_PARA",
    "HEADER_SIZE",
    "CHAPTER_ENTRY_SIZE",
    "BLOCK_HEADER_SIZE",
    "EbookWriter",
    "ChapterDraft",
    "ContentItem",
    "ContentPiece",
    "EbookFile",
    "decode_image_to_pil",
    "load_chapter",
    "load_cover_payload",
    "open_ebook",
    "CoverInfo",
    "CoverMutateResult",
    "clear_ebook_cover",
    "cover_info",
    "encode_cover_image",
    "set_ebook_cover",
    "book_id_from_extra",
    "generate_book_id",
    "normalize_book_id",
    "resolve_book_id",
    "__version__",
]
