#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QVector>

// Small authored PDFs with explicit object offsets. The optional large payload
// is a referenced, uncompressed RGB image, rather than padding after %%EOF.
namespace PdfBoundaryFixtures {

inline QString encryptedFixturePath() {
  return QDir(QString::fromUtf8(VTRPON_PDF_FIXTURE_DIR))
      .filePath(QStringLiteral("password-required.pdf"));
}

inline QString password() { return QStringLiteral("boundary-password"); }

inline bool writePdf(const QString &path, int pages, qint64 payloadBytes = 0,
                     bool complex = false, bool hugePage = false) {
  // Fixture construction itself has bounded memory and output size.
  if (pages < 0 || pages > 1001 || payloadBytes < 0 ||
      payloadBytes > 64 * 1024 * 1024 || (pages == 0 && payloadBytes != 0))
    return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const auto write = [&file](const QByteArray &bytes) {
    return file.write(bytes) == bytes.size();
  };
  const int graphicsStateObject = 3 + pages * 2;
  const int imageObject = graphicsStateObject + 1;
  const int objectCount = graphicsStateObject + (payloadBytes > 0 ? 1 : 0);
  QVector<qint64> offsets(objectCount + 1);
  const auto object = [&](int number, const QByteArray &body) {
    offsets[number] = file.pos();
    return write(QByteArray::number(number) + " 0 obj\n" + body + "\nendobj\n");
  };
  if (!write(QByteArray("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n"))) return false;
  if (!object(1, "<< /Type /Catalog /Pages 2 0 R >>")) return false;
  QByteArray kids;
  for (int page = 0; page < pages; ++page)
    kids += QByteArray::number(3 + page * 2) + " 0 R ";
  if (!object(2, "<< /Type /Pages /Count " + QByteArray::number(pages) +
                     " /Kids [ " + kids + "] >>")) return false;
  const QByteArray width = hugePage ? "1000000" : "640";
  const QByteArray height = hugePage ? "1000000" : "360";
  for (int page = 0; page < pages; ++page) {
    const int pageObject = 3 + page * 2;
    QByteArray resources = "<< /ExtGState << /GS1 " +
                           QByteArray::number(graphicsStateObject) + " 0 R >>";
    if (page == 0 && payloadBytes > 0)
      resources += " /XObject << /Im0 " + QByteArray::number(imageObject) + " 0 R >>";
    resources += " >>";
    const QByteArray rotate = complex && page % 2 == 1 ? " /Rotate 90" : "";
    if (!object(pageObject, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                                width + " " + height + "] /Resources " + resources +
                                " /Contents " + QByteArray::number(pageObject + 1) +
                                " 0 R" + rotate + " >>")) return false;
    QByteArray content = page % 2 == 0 ? "0.90196 0.12549 0.12549 rg\n"
                                      : "0.12549 0.31373 0.90196 rg\n";
    content += "0 0 " + width + " " + height + " re f\n";
    if (page == 0 && payloadBytes > 0)
      content += "q " + width + " 0 0 " + height + " 0 0 cm /Im0 Do Q\n";
    if (complex) {
      // Paths and curves stay outside the central transparency probe.
      content += "q 0.5 w 0 0 0 RG\n";
      for (int index = 0; index < 2000; ++index) {
        const auto x = QByteArray::number(10 + index % 130);
        const auto y = QByteArray::number(10 + (index * 7) % 330);
        content += x + " " + y + " m " + x + " 180 150 250 145 350 c S\n";
      }
      content += "Q\nq /GS1 gs 0 0 1 rg 160 90 320 180 re f Q\n";
    }
    if (!object(pageObject + 1, "<< /Length " + QByteArray::number(content.size()) +
                                  " >>\nstream\n" + content + "endstream")) return false;
  }
  if (!object(graphicsStateObject, "<< /Type /ExtGState /ca 0.5 /CA 0.5 >>")) return false;
  if (payloadBytes > 0) {
    constexpr int imageWidth = 4096;
    constexpr int rowBytes = imageWidth * 3;
    const qint64 imageHeight = (payloadBytes + rowBytes - 1) / rowBytes;
    const qint64 imageBytes = imageHeight * rowBytes;
    offsets[imageObject] = file.pos();
    if (!write(QByteArray::number(imageObject) +
               " 0 obj\n<< /Type /XObject /Subtype /Image /Width 4096 /Height " +
               QByteArray::number(imageHeight) +
               " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length " +
               QByteArray::number(imageBytes) + " >>\nstream\n")) return false;
    QByteArray row(rowBytes, Qt::Uninitialized);
    for (int byte = 0; byte < rowBytes; byte += 3) {
      row[byte] = char(230);
      row[byte + 1] = char(32);
      row[byte + 2] = char(32);
    }
    for (qint64 y = 0; y < imageHeight; ++y)
      if (!write(row)) return false;
    if (!write("\nendstream\nendobj\n")) return false;
  }
  const qint64 xref = file.pos();
  if (!write("xref\n0 " + QByteArray::number(objectCount + 1) +
             "\n0000000000 65535 f \n")) return false;
  for (int number = 1; number <= objectCount; ++number)
    if (!write(QByteArray::number(offsets[number]).rightJustified(10, '0') +
               " 00000 n \n")) return false;
  return write("trailer\n<< /Size " + QByteArray::number(objectCount + 1) +
               " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) +
               "\n%%EOF\n") && file.flush();
}

} // namespace PdfBoundaryFixtures
