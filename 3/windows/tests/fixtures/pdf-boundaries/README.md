# PDF boundary fixture provenance

`password-required.pdf` is an authored one-page, 640 × 360 point PDF containing
only a solid red rectangle. It has no customer material or external images,
fonts, or document content.

The fixture was generated with `generate-fixtures.py` using pypdf 6.10.0. The
test runner reads the committed PDF directly and does not require Python or
pypdf. Regeneration is optional and needs that version of pypdf.

- User password: `boundary-password`
- Owner password: `boundary-owner-password`
- Test encryption: standard RC4-128 password security, revision 3
- Fixed document identifier: `vtr-pon-boundary1`

This old encryption scheme is used solely to exercise the application's
password-required error and backend password handling. It is not a document
security recommendation. Tests confirm the fixture opens with the known
password, so malformed input cannot accidentally satisfy password-rejection
checks.

Other Windows fixtures are authored at test time by `tests/pdf_import_contract_test.cpp`.
