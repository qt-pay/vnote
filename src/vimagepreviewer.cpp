#include "vimagepreviewer.h"

#include <QTimer>
#include <QTextDocument>
#include <QDebug>
#include <QDir>
#include <QUrl>
#include "vmdedit.h"
#include "vconfigmanager.h"
#include "utils/vutils.h"
#include "vfile.h"
#include "vdownloader.h"

extern VConfigManager vconfig;

enum ImageProperty { ImagePath = 1 };

VImagePreviewer::VImagePreviewer(VMdEdit *p_edit, int p_timeToPreview)
    : QObject(p_edit), m_edit(p_edit), m_document(p_edit->document()),
      m_file(p_edit->getFile()), m_enablePreview(true), m_isPreviewing(false),
      m_requestCearBlocks(false), m_requestRefreshBlocks(false)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    Q_ASSERT(p_timeToPreview > 0);
    m_timer->setInterval(p_timeToPreview);

    connect(m_timer, &QTimer::timeout,
            this, &VImagePreviewer::timerTimeout);

    m_downloader = new VDownloader(this);
    connect(m_downloader, &VDownloader::downloadFinished,
            this, &VImagePreviewer::imageDownloaded);

    connect(m_edit->document(), &QTextDocument::contentsChange,
            this, &VImagePreviewer::handleContentChange);
}

void VImagePreviewer::timerTimeout()
{
    if (!vconfig.getEnablePreviewImages()) {
        if (m_enablePreview) {
            disableImagePreview();
        }
        return;
    }

    if (!m_enablePreview) {
        return;
    }

    previewImages();
}

void VImagePreviewer::handleContentChange(int /* p_position */,
                                          int p_charsRemoved,
                                          int p_charsAdded)
{
    if (p_charsRemoved == 0 && p_charsAdded == 0) {
        return;
    }

    m_timer->stop();
    m_timer->start();
}

void VImagePreviewer::previewImages()
{
    if (m_isPreviewing) {
        return;
    }

    m_isPreviewing = true;
    QTextBlock block = m_document->begin();
    while (block.isValid() && m_enablePreview) {
        if (isImagePreviewBlock(block)) {
            // Image preview block. Check if it is parentless.
            if (!isValidImagePreviewBlock(block)) {
                QTextBlock nblock = block.next();
                removeBlock(block);
                block = nblock;
            } else {
                block = block.next();
            }
        } else {
            clearCorruptedImagePreviewBlock(block);

            block = previewImageOfOneBlock(block);
        }
    }

    m_isPreviewing = false;

    if (m_requestCearBlocks) {
        m_requestCearBlocks = false;
        clearAllImagePreviewBlocks();
    }

    if (m_requestRefreshBlocks) {
        m_requestRefreshBlocks = false;
        refresh();
    }

    emit m_edit->statusChanged();
}

bool VImagePreviewer::isImagePreviewBlock(const QTextBlock &p_block)
{
    if (!p_block.isValid()) {
        return false;
    }

    QString text = p_block.text().trimmed();
    return text == QString(QChar::ObjectReplacementCharacter);
}

bool VImagePreviewer::isValidImagePreviewBlock(QTextBlock &p_block)
{
    if (!isImagePreviewBlock(p_block)) {
        return false;
    }

    // It is a valid image preview block only if the previous block is a block
    // need to preview (containing exactly one image) and the image paths are
    // identical.
    QTextBlock prevBlock = p_block.previous();
    if (prevBlock.isValid()) {
        QString imagePath = fetchImagePathToPreview(prevBlock.text());
        if (imagePath.isEmpty()) {
            return false;
        }

        // Get image preview block's image path.
        QString curPath = fetchImagePathFromPreviewBlock(p_block);

        return curPath == imagePath;
    } else {
        return false;
    }
}

QString VImagePreviewer::fetchImageUrlToPreview(const QString &p_text)
{
    QRegExp regExp("\\!\\[[^\\]]*\\]\\(([^\\)]+)\\)");
    int index = regExp.indexIn(p_text);
    if (index == -1) {
        return QString();
    }

    int lastIndex = regExp.lastIndexIn(p_text);
    if (lastIndex != index) {
        return QString();
    }

    return regExp.capturedTexts()[1];
}

QString VImagePreviewer::fetchImagePathToPreview(const QString &p_text)
{
    QString imageUrl = fetchImageUrlToPreview(p_text);
    if (imageUrl.isEmpty()) {
        return imageUrl;
    }

    QString imagePath;
    QFileInfo info(m_file->retriveBasePath(), imageUrl);
    if (info.exists()) {
        if (info.isNativePath()) {
            // Local file.
            imagePath = info.absoluteFilePath();
        } else {
            imagePath = imageUrl;
        }
    } else {
        QUrl url(imageUrl);
        imagePath = url.toString();
    }

    return imagePath;
}

QTextBlock VImagePreviewer::previewImageOfOneBlock(QTextBlock &p_block)
{
    if (!p_block.isValid()) {
        return p_block;
    }

    QTextBlock nblock = p_block.next();

    QString imagePath = fetchImagePathToPreview(p_block.text());
    if (imagePath.isEmpty()) {
        return nblock;
    }

    qDebug() << "block" << p_block.blockNumber() << imagePath;

    if (isImagePreviewBlock(nblock)) {
        QTextBlock nextBlock = nblock.next();
        updateImagePreviewBlock(nblock, imagePath);

        return nextBlock;
    } else {
        QTextBlock imgBlock = insertImagePreviewBlock(p_block, imagePath);

        return imgBlock.next();
    }
}

QTextBlock VImagePreviewer::insertImagePreviewBlock(QTextBlock &p_block,
                                                    const QString &p_imagePath)
{
    QString imageName = imageCacheResourceName(p_imagePath);
    if (imageName.isEmpty()) {
        return p_block;
    }

    bool modified = m_edit->isModified();

    QTextCursor cursor(p_block);
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertBlock();

    QTextImageFormat imgFormat;
    imgFormat.setName(imageName);
    imgFormat.setProperty(ImagePath, p_imagePath);
    cursor.insertImage(imgFormat);
    cursor.endEditBlock();

    V_ASSERT(cursor.block().text().at(0) == QChar::ObjectReplacementCharacter);

    m_edit->setModified(modified);

    return cursor.block();
}

void VImagePreviewer::updateImagePreviewBlock(QTextBlock &p_block,
                                              const QString &p_imagePath)
{
    QTextImageFormat format = fetchFormatFromPreviewBlock(p_block);
    V_ASSERT(format.isValid());
    QString curPath = format.property(ImagePath).toString();

    if (curPath == p_imagePath) {
        return;
    }

    // Update it with the new image.
    QString imageName = imageCacheResourceName(p_imagePath);
    if (imageName.isEmpty()) {
        // Delete current preview block.
        removeBlock(p_block);
        return;
    }

    format.setName(imageName);
    format.setProperty(ImagePath, p_imagePath);
    updateFormatInPreviewBlock(p_block, format);
}

void VImagePreviewer::removeBlock(QTextBlock &p_block)
{
    bool modified = m_edit->isModified();

    QTextCursor cursor(p_block);
    cursor.select(QTextCursor::BlockUnderCursor);
    cursor.removeSelectedText();

    m_edit->setModified(modified);
}

void VImagePreviewer::clearCorruptedImagePreviewBlock(QTextBlock &p_block)
{
    if (!p_block.isValid()) {
        return;
    }

    QString text = p_block.text();
    QVector<int> replacementChars;
    bool onlySpaces = true;
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == QChar::ObjectReplacementCharacter) {
            replacementChars.append(i);
        } else if (!text[i].isSpace()) {
            onlySpaces = false;
        }
    }

    if (!onlySpaces && !replacementChars.isEmpty()) {
        // ObjectReplacementCharacter mixed with other non-space texts.
        // Users corrupt the image preview block. Just remove the char.
        bool modified = m_edit->isModified();

        QTextCursor cursor(p_block);
        cursor.beginEditBlock();
        int blockPos = p_block.position();
        for (int i = replacementChars.size() - 1; i >= 0; --i) {
            int pos = replacementChars[i];
            cursor.setPosition(blockPos + pos);
            cursor.deleteChar();
        }
        cursor.endEditBlock();

        m_edit->setModified(modified);

        V_ASSERT(text.remove(QChar::ObjectReplacementCharacter) == p_block.text());
    }
}

bool VImagePreviewer::isPreviewEnabled()
{
    return m_enablePreview;
}

void VImagePreviewer::enableImagePreview()
{
    m_enablePreview = true;

    if (vconfig.getEnablePreviewImages()) {
        m_timer->stop();
        m_timer->start();
    }
}

void VImagePreviewer::disableImagePreview()
{
    m_enablePreview = false;

    if (m_isPreviewing) {
        // It is previewing, append the request and clear preview blocks after
        // finished previewing.
        // It is weird that when selection changed, it will interrupt the process
        // of previewing.
        m_requestCearBlocks = true;
        return;
    }

    clearAllImagePreviewBlocks();
}

void VImagePreviewer::clearAllImagePreviewBlocks()
{
    V_ASSERT(!m_isPreviewing);

    QTextBlock block = m_document->begin();
    QTextCursor cursor = m_edit->textCursor();
    bool modified = m_edit->isModified();

    cursor.beginEditBlock();
    while (block.isValid()) {
        if (isImagePreviewBlock(block)) {
            QTextBlock nextBlock = block.next();
            removeBlock(block);
            block = nextBlock;
        } else {
            clearCorruptedImagePreviewBlock(block);

            block = block.next();
        }
    }
    cursor.endEditBlock();

    m_edit->setModified(modified);

    emit m_edit->statusChanged();
}

QString VImagePreviewer::fetchImagePathFromPreviewBlock(QTextBlock &p_block)
{
    QTextImageFormat format = fetchFormatFromPreviewBlock(p_block);
    if (!format.isValid()) {
        return QString();
    }

    return format.property(ImagePath).toString();
}

QTextImageFormat VImagePreviewer::fetchFormatFromPreviewBlock(QTextBlock &p_block)
{
    QTextCursor cursor(p_block);
    int shift = p_block.text().indexOf(QChar::ObjectReplacementCharacter);
    if (shift >= 0) {
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, shift + 1);
    } else {
        return QTextImageFormat();
    }

    return cursor.charFormat().toImageFormat();
}

void VImagePreviewer::updateFormatInPreviewBlock(QTextBlock &p_block,
                                                 const QTextImageFormat &p_format)
{
    QTextCursor cursor(p_block);
    int shift = p_block.text().indexOf(QChar::ObjectReplacementCharacter);
    if (shift > 0) {
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, shift);
    }

    V_ASSERT(shift >= 0);

    cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
    V_ASSERT(cursor.charFormat().toImageFormat().isValid());

    cursor.setCharFormat(p_format);
}

QString VImagePreviewer::imageCacheResourceName(const QString &p_imagePath)
{
    V_ASSERT(!p_imagePath.isEmpty());

    auto it = m_imageCache.find(p_imagePath);
    if (it != m_imageCache.end()) {
        return it.value();
    }

    // Add it to the resource cache even if it may exist there.
    QFileInfo info(p_imagePath);
    QImage image;
    if (info.exists()) {
        // Local file.
        image = QImage(p_imagePath);
    } else {
        // URL. Try to download it.
        m_downloader->download(p_imagePath);
    }

    if (image.isNull()) {
        return QString();
    }

    QString name(imagePathToCacheResourceName(p_imagePath));
    m_document->addResource(QTextDocument::ImageResource, name, image);
    m_imageCache.insert(p_imagePath, name);

    return name;
}

QString VImagePreviewer::imagePathToCacheResourceName(const QString &p_imagePath)
{
    return p_imagePath;
}

void VImagePreviewer::imageDownloaded(const QByteArray &p_data, const QString &p_url)
{
    QImage image(QImage::fromData(p_data));

    if (!image.isNull()) {
        auto it = m_imageCache.find(p_url);
        if (it != m_imageCache.end()) {
            return;
        }

        m_timer->stop();
        QString name(imagePathToCacheResourceName(p_url));
        m_document->addResource(QTextDocument::ImageResource, name, image);
        m_imageCache.insert(p_url, name);

        qDebug() << "downloaded image cache insert" << p_url << name;

        m_timer->start();
    }
}

void VImagePreviewer::refresh()
{
    if (m_isPreviewing) {
        m_requestRefreshBlocks = true;
        return;
    }

    m_timer->stop();
    m_imageCache.clear();
    clearAllImagePreviewBlocks();
    m_timer->start();
}

QImage VImagePreviewer::fetchCachedImageFromPreviewBlock(QTextBlock &p_block)
{
    QString path = fetchImagePathFromPreviewBlock(p_block);
    if (path.isEmpty()) {
        return QImage();
    }

    auto it = m_imageCache.find(path);
    if (it == m_imageCache.end()) {
        return QImage();
    }

    return m_document->resource(QTextDocument::ImageResource, it.value()).value<QImage>();
}
