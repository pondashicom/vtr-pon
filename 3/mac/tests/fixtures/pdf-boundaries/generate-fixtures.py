"""Regenerate the authored password fixture; not required by build or CTest.

Generation dependency used: pypdf 6.10.0. All page content is authored here;
there is no imported document, font, image, or third-party source content.
"""

from pathlib import Path

from pypdf import PdfWriter
from pypdf.generic import (
    ArrayObject,
    ByteStringObject,
    DecodedStreamObject,
    NameObject,
)


def main() -> None:
    destination = Path(__file__).with_name("password-required.pdf")
    writer = PdfWriter()
    page = writer.add_blank_page(width=640, height=360)
    content = DecodedStreamObject()
    content.set_data(b"0.90196 0.12549 0.12549 rg 0 0 640 360 re f\n")
    page[NameObject("/Contents")] = writer._add_object(content)
    writer.add_metadata({"/Producer": "VTR-PON authored PDF boundary fixture"})
    # Fixed IDs make the checked-in fixture reproducible with the stated version.
    identifier = ByteStringObject(b"vtr-pon-boundary1")
    writer._ID = ArrayObject([identifier, identifier])
    writer.encrypt(
        user_password="boundary-password",
        owner_password="boundary-owner-password",
        algorithm="RC4-128",
    )
    with destination.open("wb") as output:
        writer.write(output)


if __name__ == "__main__":
    main()
