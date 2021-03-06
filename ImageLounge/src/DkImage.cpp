/*******************************************************************************************************
 DkImage.cpp
 Created on:	21.04.2011
 
 nomacs is a fast and small image viewer with the capability of synchronizing multiple instances
 
 Copyright (C) 2011-2013 Markus Diem <markus@nomacs.org>
 Copyright (C) 2011-2013 Stefan Fiel <stefan@nomacs.org>
 Copyright (C) 2011-2013 Florian Kleber <florian@nomacs.org>

 This file is part of nomacs.

 nomacs is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 nomacs is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 *******************************************************************************************************/

#include "DkImage.h"
#include "DkNoMacs.h"
#include "DkMessageBox.h"
#include "DkSettings.h"
#include "DkTimer.h"
#include "DkThumbs.h"
#include "DkImageStorage.h"
#include "DkBasicLoader.h"
#include "DkMetaData.h"
#include "DkImageContainer.h"
#include "DkMessageBox.h"
#include "DkSaveDialog.h"

#pragma warning(push, 0)	// no warnings from includes - begin
#include <QWidget>
#include <QImageWriter>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QFile>
#include <QSettings>
#include <QImageReader>
#include <QDir>
#include <QThread>
#include <QBuffer>
#include <QStringBuilder>
#include <QDebug>
#include <QMutex>
#include <QFileIconProvider>
#include <QStringList>
#include <QMessageBox>
#include <QDirIterator>
#include <QProgressDialog>
#include <QReadLocker>
#include <QWriteLocker>
#include <QReadWriteLock>
#include <QTimer>
#include <QMovie>
#include <QByteArray>
#include <QCoreApplication>
#include <QApplication>
#include <QPluginLoader>
#include <QFileDialog>
#include <QPainter>
#include <qmath.h>
#include <QtConcurrentRun>

// quazip
#ifdef WITH_QUAZIP
#include <quazip/JlCompress.h>
#endif

// opencv
#ifdef WITH_OPENCV

#ifdef WIN32
#pragma warning(disable: 4996)
#endif
#endif 

#ifdef WITH_LIBRAW
#include <libraw/libraw.h>
#elif defined(WIN32) && !defined(SOCK_STREAM)
#include <winsock2.h>	// needed since libraw 0.16
#endif

#pragma warning(pop)	// no warnings from includes - end

namespace nmc {

// DkImageLoader -> is nomacs file handling routine --------------------------------------------------------------------
/**
 * Default constructor.
 * Creates a DkImageLoader instance with a given file.
 * @param file the file to be loaded.
 **/ 
DkImageLoader::DkImageLoader(QFileInfo file) {

	qRegisterMetaType<QFileInfo>("QFileInfo");

	dirWatcher = new QFileSystemWatcher(this);
	connect(dirWatcher, SIGNAL(directoryChanged(QString)), this, SLOT(directoryChanged(QString)));

	sortingIsDirty = false;
	sortingImages = false;
	folderUpdated = false;
	tmpFileIdx = 0;

	connect(&createImageWatcher, SIGNAL(finished()), this, SLOT(imagesSorted()));

	delayedUpdateTimer.setSingleShot(true);
	connect(&delayedUpdateTimer, SIGNAL(timeout()), this, SLOT(directoryChanged()));
	timerBlockedUpdate = false;

	//saveDir = DkSettings::global.lastSaveDir;	// loading save dir is obsolete ?!
	saveDir = "";
	 
	if (file.exists())
		loadDir(file);
	else
		dir = DkSettings::global.lastDir;
}

/**
 * Default destructor.
 **/ 
DkImageLoader::~DkImageLoader() {
	
	if (createImageWatcher.isRunning())
		createImageWatcher.blockSignals(true);
}

/**
 * Clears the path.
 * Calling this method makes the loader forget
 * about the current directory. It also destroys
 * the currently loaded image.
 **/ 
void DkImageLoader::clearPath() {

	// lastFileLoaded must exist
	if (currentImage && currentImage->exists()) {
		currentImage->receiveUpdates(this, false);
		lastImageLoaded = currentImage;
		images.clear();
	}

	currentImage.clear();
}

#ifdef WITH_QUAZIP
/**
 * Loads a given zip archive and the first image in it.
 * @param zipFile the archive to be loaded.
 **/ 
bool DkImageLoader::loadZipArchive(QFileInfo zipFile) {

	QStringList fileNameList = JlCompress::getFileList(zipFile.absoluteFilePath());
	
	// remove the * in fileFilters
	QStringList fileFiltersClean = DkSettings::app.browseFilters;
	for (int idx = 0; idx < fileFiltersClean.size(); idx++)
		fileFiltersClean[idx].replace("*", "");

	QStringList fileList;
	for (int idx = 0; idx < fileNameList.size(); idx++) {
		
		for (int idxFilter = 0; idxFilter < fileFiltersClean.size(); idxFilter++) {

			if (fileNameList.at(idx).contains(fileFiltersClean[idxFilter], Qt::CaseInsensitive)) {
				fileList.append(fileNameList.at(idx));
				break;
			}
		}
	}

	QFileInfoList fileInfoList;
	//encode both the input zip file and the output image into a single fileInfo
	for (int idx = 0; idx < fileList.size(); idx++)
		fileInfoList.append(DkZipContainer::encodeZipFile(zipFile, fileList.at(idx)));

	images.clear();
	createImages(fileInfoList);
	
	// zip archives could not contain known image formats
	if (fileInfoList.empty()) {
		emit showInfoSignal(tr("%1 \n does not contain any image").arg(zipFile.fileName()), 4000);	// stop showing
		return false;
	}

	emit updateDirSignal(images);
	dir = zipFile.absoluteDir();

	return true;
}
#endif

bool DkImageLoader::loadDir(QFileInfo newFile, bool scanRecursive /* = true */) {

#ifdef WITH_QUAZIP
	bool isZipArchive = DkBasicLoader::isContainer(newFile);

	if (isZipArchive)
		return loadZipArchive(newFile);
#endif

	newFile.refresh();
	if (!newFile.exists())
		return false;

	return loadDir(newFile.absoluteDir(), scanRecursive);
}

/**
 * Loads a given directory.
 * @param newDir the directory to be loaded.
 **/ 
bool DkImageLoader::loadDir(QDir newDir, bool scanRecursive) {

	//if (creatingImages) {
	//	//emit showInfoSignal(tr("Indexing folder..."), 4000);	// stop showing
	//	return false;
	//}

	// folder changed signal was emitted
	if (folderUpdated && newDir.absolutePath() == dir.absolutePath()) {
		
		folderUpdated = false;
		QFileInfoList files = getFilteredFileInfoList(dir, ignoreKeywords, keywords, folderKeywords);		// this line takes seconds if you have lots of files and slow loading (e.g. network)

		// might get empty too (e.g. someone deletes all images)
 		if (files.empty()) {
			emit showInfoSignal(tr("%1 \n does not contain any image").arg(dir.absolutePath()), 4000);	// stop showing
			images.clear();
			emit updateDirSignal(images);
			return false;
		}

		// disabled threaded sorting - people didn't like it (#484 and #460)
		//if (files.size() > 2000) {
		//	createImages(files, false);
		//	sortImagesThreaded(images);
		//}
		//else
			createImages(files, true);

		qDebug() << "getting file list.....";
	}
	// new folder is loaded
	else if ((newDir.absolutePath() != dir.absolutePath() || images.empty()) && newDir.exists()) {

		QFileInfoList files;

		// update save directory
		dir = newDir;
		dir.setNameFilters(DkSettings::app.fileFilters);
		dir.setSorting(QDir::LocaleAware);		// TODO: extend
		folderUpdated = false;

		folderKeywords.clear();	// delete key words -> otherwise user may be confused
		emit folderFiltersChanged(folderKeywords);

		if (scanRecursive && DkSettings::global.scanSubFolders)
			files = updateSubFolders(dir);
		else 
			files = getFilteredFileInfoList(dir, ignoreKeywords, keywords, folderKeywords);		// this line takes seconds if you have lots of files and slow loading (e.g. network)

		if (files.empty()) {
			emit showInfoSignal(tr("%1 \n does not contain any image").arg(dir.absolutePath()), 4000);	// stop showing
			return false;
		}

		// ok new folder, this should speed-up loading
		images.clear();
		
		// TODO: creating ~120 000 images takes about 2 secs
		// but sorting (just filenames) takes ages (on windows)
		// so we should fix this using 2 strategies: 
		// - thread the image creation process
		// - while loading (if the user wants to move in the folder) we could display some message (e.g. indexing dir)
		if (files.size() > 2000) {
			createImages(files, false);
			sortImagesThreaded(images);
		}
		else
			createImages(files, true);

		qDebug() << "new folder path: " << newDir.absolutePath() << " contains: " << images.size() << " images";
	}
	//else
	//	qDebug() << "ignoring... old dir: " << dir.absolutePath() << " newDir: " << newDir << " file size: " << images.size();

	return true;
}

void DkImageLoader::sortImagesThreaded(QVector<QSharedPointer<DkImageContainerT > > images) {

	if (sortingImages) {
		sortingIsDirty = true;
		return;
	}

	sortingIsDirty = false;
	sortingImages = true;
	createImageWatcher.setFuture(QtConcurrent::run(this, 
		&nmc::DkImageLoader::sortImages, images));

	qDebug() << "sorting images threaded...";
}

void DkImageLoader::imagesSorted() {

	sortingImages = false;
	images = createImageWatcher.result();

	if (sortingIsDirty) {
		qDebug() << "re-sorting because it's dirty...";
		sortImagesThreaded(images);
		return;
	}

	emit updateDirSignal(images);

	if (dirWatcher) {
		if (!dirWatcher->directories().isEmpty())
			dirWatcher->removePaths(dirWatcher->directories());
		dirWatcher->addPath(dir.absolutePath());
	}

	qDebug() << "images sorted...";
}

void DkImageLoader::createImages(const QFileInfoList& files, bool sort) {

	DkTimer dt;
	QVector<QSharedPointer<DkImageContainerT > > oldImages = images;
	images.clear();

	for (int idx = 0; idx < files.size(); idx++) {

		int oIdx = findFileIdx(files.at(idx), oldImages);

		if (oIdx != -1 && oldImages.at(oIdx)->file().lastModified() == files.at(idx).lastModified())
			images.append(oldImages.at(oIdx));
		else
			images.append(QSharedPointer<DkImageContainerT >(new DkImageContainerT(files.at(idx))));
	}
	qDebug() << "[DkImageLoader] " << images.size() << " containers created in " << dt.getTotal();

	if (sort) {
		qSort(images.begin(), images.end(), imageContainerLessThanPtr);
		qDebug() << "[DkImageLoader] after sorting: " << dt.getTotal();

		emit updateDirSignal(images);

		if (dirWatcher) {
			if (!dirWatcher->directories().isEmpty())
				dirWatcher->removePaths(dirWatcher->directories());
			dirWatcher->addPath(dir.absolutePath());
		}
	}

}

QVector<QSharedPointer<DkImageContainerT > > DkImageLoader::sortImages(QVector<QSharedPointer<DkImageContainerT > > images) const {

	qSort(images.begin(), images.end(), imageContainerLessThanPtr);

	return images;
}

/**
 * Loads the ancesting or subsequent file.
 * @param skipIdx the number of files that should be skipped after/before the current file.
 * @param silent if true, no status information will be displayed.
 **/ 
void DkImageLoader::changeFile(int skipIdx) {

	//if (!img.isNull() && !file.exists())
	//	return;
	//if (!file.exists() && !virtualFile.exists()) {
	//	qDebug() << virtualFile.absoluteFilePath() << "does not exist...!!!";
	//	return;
	//}

	if (skipIdx == 0) {
		reloadImage();
		return;
	}

	// update dir
	loadDir(dir);

	QSharedPointer<DkImageContainerT> imgC = getSkippedImage(skipIdx);

	load(imgC);
}

/**
 * Returns the file info of the ancesting/subsequent file + skipIdx.
 * @param skipIdx the number of files to be skipped from the current file.
 * @param silent if true, no status information will be displayed.
 * @return QFileInfo the file info of the demanded file
 **/ 
QSharedPointer<DkImageContainerT> DkImageLoader::getSkippedImage(int skipIdx, bool searchFile, bool recursive) {

	QSharedPointer<DkImageContainerT> imgC;

	if (!currentImage)
		return imgC;

	DkTimer dt;


	// load a page (e.g. within a tiff file)
	if (currentImage->setPageIdx(skipIdx))
		return currentImage;

	//if (searchFile && currentImage->file().absoluteDir() != dir.absolutePath()) {
	//	qDebug() << "loading new dir: " << currentImage->file().absolutePath();
	//	qDebug() << "old dir: " << dir.absolutePath();
	
	if (!recursive)
		loadDir(currentImage->file(), false);

	// locate the current file
	int newFileIdx = 0;
	
	if (searchFile) tmpFileIdx = 0;

	//qDebug() << "virtual file " << virtualFile.absoluteFilePath();
	//qDebug() << "file" << file.absoluteFilePath();

	//if (virtualExists || file.exists()) {

	if (searchFile) {

		QFileInfo file = (currentImage->exists()) ? currentImage->file() : DkSettings::global.recentFiles.first();

		tmpFileIdx = findFileIdx(file, images);

		// could not locate the file -> it was deleted?!
		if (tmpFileIdx == -1) {

			tmpFileIdx = 0;
			for (; tmpFileIdx < images.size(); tmpFileIdx++) {

				if (*(currentImage.data()) < *(images[tmpFileIdx].data()))
					break;
			}

			if (skipIdx > 0)
				tmpFileIdx--;	// -1 because the current file does not exist
			if (images.size() == tmpFileIdx)	// could not locate file - resize
				tmpFileIdx = 0;

		}		

	}
	newFileIdx = tmpFileIdx + skipIdx;

	//qDebug() << "subfolders: " << DkSettings::global.scanSubFolders << "subfolder size: " << (subFolders.size() > 1);

	if (DkSettings::global.scanSubFolders && subFolders.size() > 1 && (newFileIdx < 0 || newFileIdx >= images.size())) {

		int folderIdx = 0;

		// locate folder
		for (int idx = 0; idx < subFolders.size(); idx++) {
			if (subFolders[idx] == dir.absolutePath()) {
				folderIdx = idx;
				break;
			}
		}

		if (newFileIdx < 0)
			folderIdx = getPrevFolderIdx(folderIdx);
		else
			folderIdx = getNextFolderIdx(folderIdx);

		qDebug() << "new folder idx: " << folderIdx;
			
		//if (DkSettings::global.loop)
		//	folderIdx %= subFolders.size();

		if (folderIdx >= 0 && folderIdx < subFolders.size()) {
				
			int oldFileSize = images.size();
			loadDir(QDir(subFolders[folderIdx]), false);	// don't scan recursive again
			qDebug() << "loading new folder: " << subFolders[folderIdx];

			if (newFileIdx >= oldFileSize) {
				newFileIdx -= oldFileSize;
				tmpFileIdx = 0;
				qDebug() << "new skip idx: " << newFileIdx << "cFileIdx: " << tmpFileIdx << " -----------------------------";
				getSkippedImage(newFileIdx, false, true);
			}
			else if (newFileIdx < 0) {
				newFileIdx += tmpFileIdx;
				tmpFileIdx = images.size()-1;
				qDebug() << "new skip idx: " << newFileIdx << "cFileIdx: " << tmpFileIdx << " -----------------------------";
				getSkippedImage(newFileIdx, false, true);
			}
		}
		//// dir up
		//else if (folderIdx == subFolders.size()) {

		//	qDebug() << "going one up";
		//	dir.cd("..");
		//	loadDir(dir, false);	// don't scan recursive again
		//	newFileIdx += cFileIdx;
		//	cFileIdx = 0;
		//	getChangedFileInfo(newFileIdx, silent, false);
		//}
		//// get root files
		//else if (folderIdx < 0) {
		//	loadDir(dir, false);
		//}

	}

	// this should never happen!
	if (images.empty()) {
		qDebug() << "file list is empty, where it should not be";
		return imgC;
	}

	// loop the directory
	if (DkSettings::global.loop) {
		newFileIdx %= images.size();

		while (newFileIdx < 0)
			newFileIdx = images.size() + newFileIdx;

	}
	// clip to pos1 if skipIdx < -1
	else if (tmpFileIdx > 0 && newFileIdx < 0) {
		newFileIdx = 0;
	}
	// clip to end if skipIdx > 1
	else if (tmpFileIdx < images.size()-1 && newFileIdx >= images.size()) {
		newFileIdx = images.size()-1;
	}
	// tell user that there is nothing left to display
	else if (newFileIdx < 0) {

		// TODO: find an elegant way to switch back to the zip folder
		QString msg = tr("You have reached the beginning");
		showInfoSignal(msg, 1000);
		return imgC;
	}
	// tell user that there is nothing left to display
	else if (newFileIdx >= images.size()) {
		QString msg = tr("You have reached the end");
			
		if (!DkSettings::global.loop)
			emit setPlayer(false);

		showInfoSignal(msg, 1000);
		return imgC;
	}
	//}

	//qDebug() << "file idx changed in: " << QString::fromStdString(dt.getTotal());

	tmpFileIdx = newFileIdx;

	if (newFileIdx >= 0 && newFileIdx < images.size())
		imgC = images.at(newFileIdx);


	// file requested becomes current file
	return imgC;
	
}


/**
* Loads the file at index idx.
* @param idx the file index of the file which should be loaded.
**/ 
void DkImageLoader::loadFileAt(int idx) {

	//if (basicLoader.hasImage() && !file.exists())
	//	return;

	if (currentImage && !dir.exists())
		loadDir(currentImage->file());

	if(images.empty())
		return;

	if (dir.exists()) {

		if (idx == -1) {
			idx = images.size()-1;
		}
		else if (DkSettings::global.loop) {
			idx %= images.size();

			while (idx < 0)
				idx = images.size() + idx;

		}
		else if (idx < 0 && !DkSettings::global.loop) {
			QString msg = tr("You have reached the beginning");
			emit showInfoSignal(msg, 1000);
			return;
		}
		else if (idx >= images.size()) {
			QString msg = tr("You have reached the end");
			if (!DkSettings::global.loop)
				emit(setPlayer(false));
			emit showInfoSignal(msg, 1000);
			return;
		}
	}

	// file requested becomes current file
	setCurrentImage(images.at(idx));

	load(currentImage);
}

QSharedPointer<DkImageContainerT> DkImageLoader::findOrCreateFile(const QFileInfo& file) const {

	QSharedPointer<DkImageContainerT> imgC = findFile(file);

	if (!imgC)
		imgC = QSharedPointer<DkImageContainerT>(new DkImageContainerT(file));

	return imgC;
}

QSharedPointer<DkImageContainerT> DkImageLoader::findFile(const QFileInfo& file) const {

	// if one image is from zip than all should be
	// for images in zip the "images[idx]->file() == file" comparison somahow does not work
	if(images.size() > 0) {

		if (images[0]->isFromZip()) {
			int idx = findFileIdx(file, images);
			if (idx < 0) 
				return QSharedPointer<DkImageContainerT>();
			else 
				return images[idx];
		}
	}

	for (int idx = 0; idx < images.size(); idx++) {

		if (images[idx]->file() == file)
			return images[idx];
	}

	return QSharedPointer<DkImageContainerT>();
}

int DkImageLoader::findFileIdx(const QFileInfo& file, const QVector<QSharedPointer<DkImageContainerT> >& images) const {

	for (int idx = 0; idx < images.size(); idx++) {

		if (images[idx]->file().absoluteFilePath() == file.absoluteFilePath())
			return idx;
	}

	return -1;
}


//
///**
// * Returns all files of the current directory.
// * @return QStringList empty list if no directory is set.
// **/ 
//QStringList DkImageLoader::getFiles() {
//
//	// guarantee that the file list is up-to-date
//	loadDir(dir);
//	
//	return files;
//}

QStringList DkImageLoader::getFileNames() {

	QStringList fileNames;

	for (int idx = 0; idx < images.size(); idx++)
		fileNames.append(images[idx]->file().fileName());

	return fileNames;
}

QVector<QSharedPointer<DkImageContainerT> > DkImageLoader::getImages() {

	loadDir(dir);
	return images;
}

void DkImageLoader::setImages(QVector<QSharedPointer<DkImageContainerT> > images) {

	this->images = images;
	emit updateDirSignal(images);
}

/**
 * Loads the first file of the current directory.
 **/ 
void DkImageLoader::firstFile() {

	loadFileAt(0);
}

/**
 * Loads the last file of the current directory.
 **/ 
void DkImageLoader::lastFile() {
	
	loadFileAt(-1);
}

bool DkImageLoader::unloadFile() {

	if (!currentImage)
		return true;

	// if we are either in rc or remote display mode & the directory does not exist - we received an image, so don't ask the user
	if (currentImage->isEdited() && (DkSettings::sync.syncMode == DkSettings::sync_mode_default)) {
		DkMessageBox* msgBox = new DkMessageBox(QMessageBox::Question, tr("Save Image"), tr("Do you want to save changes to:\n%1").arg(currentImage->file().fileName()), 
			(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel), DkNoMacs::getDialogParent());

		msgBox->setDefaultButton(QMessageBox::No);
		msgBox->setObjectName("saveEditDialog");

		int answer = msgBox->exec();

		// TODO: Save As dialog for unsupported files
		if (answer == QMessageBox::Accepted || answer == QMessageBox::Yes) {
			currentImage->saveImageThreaded(currentImage->file());
		}
		else if (answer == QMessageBox::Cancel) {
			return false;
		}
		else {
			currentImage->saveMetaDataThreaded();
		}
	}

	return true;
}

/**
 * Convenience function see @activate.
 **/ 
void DkImageLoader::deactivate() {

	activate(false);
}


/**
 * Activates or deactivates the loader.
 * If activated, the directory is indexed & the current image is loaded.
 * If deactivated, the image list & the current image are deleted which
 * should save some memory. In addition, all signals are blocked.
 * @param isActive if true, the loader is activated
 **/ 
void DkImageLoader::activate(bool isActive /* = true */) {

	if (!isActive) {
		// go to sleep - schlofand w�hlar ihr cam�lar
		blockSignals(true);
		clearPath();
	}
	else if (!currentImage) {
		// wake up again
		blockSignals(false);
		setCurrentImage(lastImageLoaded);
	}
}

void DkImageLoader::setCurrentImage(QSharedPointer<DkImageContainerT> newImg) {

	if (signalsBlocked()) {
		currentImage = newImg;
		return;
	}

	if (newImg)
		loadDir(newImg->file());
	else
		qDebug() << "empty image assigned";
	
	// if the file stays the same, we just want to update the pointer
	bool updatePointer = newImg && currentImage && newImg->file().absoluteFilePath() == currentImage->file().absoluteFilePath();

	// cancel action if the image is currently loading
	if (DkSettings::resources.waitForLastImg &&
		currentImage && currentImage->getLoadState() == DkImageContainerT::loading && 
		newImg && newImg->file().absoluteDir() == currentImage->file().absoluteDir())
		return;

	if (currentImage) {

		// do we load a new image?
		if (!updatePointer) {
			currentImage->cancel();

			if (currentImage->getLoadState() == DkImageContainer::loading_canceled)
				emit showInfoSignal(newImg->file().fileName(), 3000, 1);

			// this causes a crash 
			// because the release will trigger the saving of
			// metadata (in a different thread) - and
			// the auto_ptr does not like that at all
			// anyhow we don't need to save the metadata twice
			//currentImage->saveMetaDataThreaded();

			if (!DkSettings::resources.cacheMemory)
				currentImage->clear();

			currentImage->getLoader()->resetPageIdx();
		}
		currentImage->receiveUpdates(this, false);	// reset updates
	}

	currentImage = newImg;

	if (currentImage)
		currentImage->receiveUpdates(this);
}

void DkImageLoader::reloadImage() {

	if(!currentImage)
		return;

	if (!currentImage->exists()) {
		// message when reloaded
		QString msg = tr("sorry, %1 does not exist anymore...").arg(currentImage->file().fileName());
		emit showInfoSignal(msg, 4000);
		return;
	}

	dir = QDir();
	images.clear();
	currentImage->clear();
	setCurrentImage(currentImage);
	load(currentImage);
}

void DkImageLoader::load(const QFileInfo& file) {

	bool hasZipMarker = false;

#ifdef WITH_QUAZIP
	hasZipMarker = file.absoluteFilePath().contains(DkZipContainer::zipMarker()) != 0;
#endif

	loadDir(file);

	if (file.isFile() || hasZipMarker) {
		QSharedPointer<DkImageContainerT> newImg = findOrCreateFile(file);
		setCurrentImage(newImg);
		load(currentImage);
	}
	else
		firstFile();
	
}

void DkImageLoader::load(QSharedPointer<DkImageContainerT> image /* = QSharedPointer<DkImageContainerT> */) {

	if (!image)
		return;

#ifdef WITH_QUAZIP
	bool isZipArchive = DkBasicLoader::isContainer(image->file());

	if (isZipArchive) {
		loadZipArchive(image->file());
		firstFile();
		return;
	}
#endif

	setCurrentImage(image);

	if (currentImage && currentImage->getLoadState() == DkImageContainerT::loading)
		return;

	emit updateSpinnerSignalDelayed(true);
	bool loaded = currentImage->loadImageThreaded();	// loads file threaded
	
	if (!loaded)
		emit updateSpinnerSignalDelayed(false);

	// if loaded is false, we definitively know that the file does not exist -> early exception here?
}

void DkImageLoader::imageLoaded(bool loaded /* = false */) {
	
	emit updateSpinnerSignalDelayed(false);

	if (!currentImage) {
		return;
	}

	emit imageLoadedSignal(currentImage, loaded);

	if (!loaded)
		return;

	emit imageUpdatedSignal(currentImage);

	if (currentImage) {
		emit updateFileSignal(currentImage->file());

		// this signal is needed by the folder scrollbar
		int idx = findFileIdx(currentImage->file(), images);
		emit imageUpdatedSignal(idx);
	}

	QApplication::sendPostedEvents();	// force an event post here

	if (currentImage && currentImage->isFileDownloaded())
		saveTempFile(currentImage->image());

	updateCacher(currentImage);
	updateHistory();

	if (currentImage)
		emit imageHasGPSSignal(DkMetaDataHelper::getInstance().hasGPS(currentImage->getMetaData()));
}

void DkImageLoader::downloadFile(const QUrl& url) {

	QSharedPointer<DkImageContainerT> newImg = findOrCreateFile(QFileInfo());
	setCurrentImage(newImg);
	newImg->downloadFile(url);
	newImg->setEdited(true);
	emit updateSpinnerSignalDelayed(true);
}

/**
 * Saves a temporary file to the folder specified in Settings.
 * @param img the image (which was in most cases pasted to nomacs)
 **/ 
QFileInfo DkImageLoader::saveTempFile(QImage img, QString name, QString fileExt, bool force, bool threaded) {

	// do not save temp images if we are remote control or remote display
	if (DkSettings::sync.syncMode != DkSettings::sync_mode_default)
		return QFileInfo();

	QFileInfo tmpPath = QFileInfo(DkSettings::global.tmpPath + "\\");
	
	if (!force && (!DkSettings::global.useTmpPath || !tmpPath.exists())) {
		qDebug() << tmpPath.absolutePath() << "does not exist";
		return QFileInfo();
	}
	else if ((!DkSettings::global.useTmpPath || !tmpPath.exists())) {

#ifdef WIN32
		
		// TODO: this path seems to be perfectly ok (you can copy it to windows explorer) - however Qt thinks it does not exist??
		QString defaultPath = getenv("HOMEPATH");
		defaultPath = "C:" + defaultPath + "\\My Pictures\\";
		tmpPath = defaultPath;

		qDebug() << "default path: " << tmpPath.absoluteFilePath();
#endif

		if (!tmpPath.isDir()) {
			// load system default open dialog
			QString dirName = QFileDialog::getExistingDirectory(DkNoMacs::getDialogParent(), tr("Save Directory"),
				getDir().absolutePath());

			tmpPath = dirName + "/";

			if (!tmpPath.exists())
				return QFileInfo();
		}
	}

	qDebug() << "tmpPath: " << tmpPath.absolutePath();
	
	QString fileName = name + "-" + QDateTime::currentDateTime().toString("yyyy-MM-dd hh.mm.ss") + fileExt;
	QFileInfo tmpFile = QFileInfo(tmpPath.absolutePath(), fileName);

	if (!tmpFile.exists()) {
			
		saveFile(tmpFile, img, "", -1, threaded);
		return tmpFile;
	}

	return QFileInfo();
}

void DkImageLoader::saveFileWeb(QImage saveImg) {
	
	QString saveName;
	QFileInfo saveFileInfo;

	if (hasFile()) {
		saveFileInfo = file();
		saveName = saveFileInfo.fileName();

		qDebug() << "save dir: " << getSaveDir();

		if (getSaveDir() != saveFileInfo.absoluteDir())
			saveFileInfo = QFileInfo(getSaveDir(), saveName);
	}

	bool imgHasAlpha = DkImage::alphaChannelUsed(saveImg);

	QString suffix = imgHasAlpha ? ".png" : ".jpg";
	QString saveFilterGui;

	for (int idx = 0; idx < DkSettings::app.saveFilters.size(); idx++) {

		if (DkSettings::app.saveFilters.at(idx).contains(suffix)) {
			saveFilterGui = DkSettings::app.saveFilters.at(idx);
			break;
		}
	}

	if (saveFileInfo.exists())
		saveFileInfo = QFileInfo(saveFileInfo.absolutePath(), saveFileInfo.baseName() + suffix);

	QString fileName = QFileDialog::getSaveFileName(qApp->activeWindow(), tr("Save File %1").arg(saveName),
		saveFileInfo.absoluteFilePath(), saveFilterGui);

	if (fileName.isEmpty())
		return;

	DkCompressDialog* jpgDialog = new DkCompressDialog(qApp->activeWindow());
	jpgDialog->setDialogMode(DkCompressDialog::web_dialog);
	jpgDialog->imageHasAlpha(imgHasAlpha);
	jpgDialog->setImage(&saveImg);

	if (!jpgDialog->exec())
		return;

	float factor = jpgDialog->getResizeFactor();
	if (factor != -1)
		saveImg = DkImage::resizeImage(saveImg, QSize(), factor, DkImage::ipl_area);

	saveFile(fileName, saveImg, suffix, jpgDialog->getCompression());

	jpgDialog->deleteLater();
}

void DkImageLoader::saveUserFileAs(QImage saveImg, bool silent) {

	QString selectedFilter;
	QString saveName;
	QFileInfo saveFile;

	if (hasFile()) {
		saveFile = file();
		saveName = saveFile.fileName();

		qDebug() << "save dir: " << getSaveDir();

		if (getSaveDir() != saveFile.absoluteDir())
			saveFile = QFileInfo(getSaveDir(), saveName);

		int filterIdx = -1;

		QStringList sF = DkSettings::app.saveFilters;
		//qDebug() << sF;

		QRegExp exp = QRegExp("*." + saveFile.suffix() + "*", Qt::CaseInsensitive);
		exp.setPatternSyntax(QRegExp::Wildcard);

		for (int idx = 0; idx < sF.size(); idx++) {

			if (exp.exactMatch(sF.at(idx))) {
				selectedFilter = sF.at(idx);
				filterIdx = idx;
				break;
			}
		}

		if (filterIdx == -1)
			saveName.remove("." + saveFile.suffix());
	}

	QString fileName;

	int answer = QDialog::Rejected;

	// don't ask the user if save was hit & the file format is supported for saving
	if (silent && !selectedFilter.isEmpty() && isEdited()) {
		fileName = file().absoluteFilePath();
		DkMessageBox* msg = new DkMessageBox(QMessageBox::Question, tr("Overwrite File"), 
			tr("Do you want to overwrite:\n%1?").arg(fileName), 
			(QMessageBox::Yes | QMessageBox::No), qApp->activeWindow());
		msg->setObjectName("overwriteDialog");

		//msg->show();
		answer = msg->exec();

	}
	if (answer == QDialog::Rejected || answer == QMessageBox::No) {
		// note: basename removes the whole file name from the first dot...
		QString savePath = (!selectedFilter.isEmpty()) ? saveFile.absoluteFilePath() : QFileInfo(saveFile.absoluteDir(), saveName).absoluteFilePath();

		fileName = QFileDialog::getSaveFileName(qApp->activeWindow(), tr("Save File %1").arg(saveName),
			savePath, DkSettings::app.saveFilters.join(";;"), &selectedFilter);
	}


	//qDebug() << "selected Filter: " << selectedFilter;


	// uncomment if you want to use native Qt dialog -> slow! ------------------------------------	
	//if (!saveDialog)
	//	saveDialog = new QFileDialog(this);

	//saveDialog->setWindowTitle(tr("Save Image"));
	//saveDialog->setAcceptMode(QFileDialog::AcceptSave);
	//saveDialog->setFilters(DkImageLoader::saveFilters);
	//
	//if (loader->hasFile()) {
	//	saveDialog->selectNameFilter(loader->getCurrentFilter());	// select the current file filter
	//	saveDialog->setDirectory(loader->getSaveDir());
	//	saveDialog->selectFile(loader->getFile().fileName());
	//}

	//saveDialog->show();

	//// show the dialog
	//if(saveDialog->exec())
	//	fileNames = saveDialog->selectedFiles();

	//if (fileNames.empty())
	//	return;
	// Uncomment if you want to use native Qt dialog -> slow! ------------------------------------

	if (fileName.isEmpty())
		return;


	QString ext = QFileInfo(fileName).suffix();

	if (!ext.isEmpty() && !selectedFilter.contains(ext)) {

		QStringList sF = DkSettings::app.saveFilters;

		for (int idx = 0; idx < sF.size(); idx++) {

			if (sF.at(idx).contains(ext)) {
				selectedFilter = sF.at(idx);
				break;
			}
		}
	}

	// TODO: if more than one file is opened -> open new threads
	QFileInfo sFile = QFileInfo(fileName);
	int compression = -1;	// default value

	DkCompressDialog* jpgDialog = 0;

	if (selectedFilter.contains(QRegExp("(jpg|jpeg|j2k|jp2|jpf|jpx)", Qt::CaseInsensitive))) {
		
		if (!jpgDialog)
			jpgDialog = new DkCompressDialog(qApp->activeWindow());

		if (selectedFilter.contains(QRegExp("(j2k|jp2|jpf|jpx)")))
			jpgDialog->setDialogMode(DkCompressDialog::j2k_dialog);
		else
			jpgDialog->setDialogMode(DkCompressDialog::jpg_dialog);

		jpgDialog->imageHasAlpha(saveImg.hasAlphaChannel());
		//jpgDialog->show();
		jpgDialog->setImage(&saveImg);


		if (!jpgDialog->exec())
			return;

		compression = jpgDialog->getCompression();

		if (saveImg.hasAlphaChannel()) {

			QRect imgRect = QRect(QPoint(), saveImg.size());
			QImage tmpImg = QImage(saveImg.size(), QImage::Format_RGB32);
			QPainter painter(&tmpImg);
			painter.fillRect(imgRect, jpgDialog->getBackgroundColor());
			painter.drawImage(imgRect, saveImg, imgRect);

			saveImg = tmpImg;
		}

		//	qDebug() << "returned: " << ret;
	}

	if (selectedFilter.contains("webp")) {

		if (!jpgDialog)
			jpgDialog = new DkCompressDialog(qApp->activeWindow());

		jpgDialog->setDialogMode(DkCompressDialog::webp_dialog);

		jpgDialog->setImage(&saveImg);

		if (!jpgDialog->exec())
			return;

		compression = jpgDialog->getCompression();
	}

	DkTifDialog* tifDialog = 0;

	//if (saveDialog->selectedNameFilter().contains("tif")) {
	if (selectedFilter.contains("tif")) {

		if (!tifDialog)
			tifDialog = new DkTifDialog(qApp->activeWindow());

		if (!tifDialog->exec())
			return;

		compression = tifDialog->getCompression();
	}

	this->saveFile(sFile, saveImg, selectedFilter, compression);

	if (tifDialog)
		tifDialog->deleteLater();
	if (jpgDialog)
		jpgDialog->deleteLater();

}

/**
 * Saves a file (not threaded!)
 * If the file already exists, it will be replaced.
 * @param file the file name/path
 * @param fileFilter the file extension (e.g. *.jpg)
 * @param saveImg the image to be saved
 * @param compression the compression method (for jpg, tif)
 **/ 
void DkImageLoader::saveFile(QFileInfo file, QImage saveImg, QString fileFilter, int compression, bool threaded) {
	
	QSharedPointer<DkImageContainerT> imgC = (currentImage) ? currentImage : findOrCreateFile(file);
	setCurrentImage(imgC);

	if (saveImg.isNull() && (!currentImage || !currentImage->hasImage()))
		emit showInfoSignal(tr("Sorry, I cannot save an empty image..."));

	QString filePath = file.absoluteFilePath();

	// if the user did not specify the suffix - append the suffix of the file filter
	QString newSuffix = file.suffix();
	if (newSuffix == "") {
		
		newSuffix = fileFilter.remove(0, fileFilter.indexOf("."));
		printf("new suffix: %s\n", newSuffix.toStdString().c_str());

		int endSuffix = -1;
		if (newSuffix.indexOf(")") == -1)
			endSuffix =  newSuffix.indexOf(" ");
		else if (newSuffix.indexOf(" ") == -1)
			endSuffix =  newSuffix.indexOf(")");
		else
			endSuffix = qMin(newSuffix.indexOf(")"), newSuffix.indexOf(" "));

		filePath.append(newSuffix.left(endSuffix));
		file = QFileInfo(filePath);
	}

	emit updateSpinnerSignalDelayed(true);
	QImage sImg = (saveImg.isNull()) ? imgC->image() : saveImg;

	qDebug() << "saving: " << file.absoluteFilePath();

	dirWatcher->blockSignals(true);
	bool saveStarted = (threaded) ? imgC->saveImageThreaded(file, sImg, compression) : imgC->saveImage(file, sImg, compression);

	if (!saveStarted) {
		dirWatcher->blockSignals(false);
		imageSaved(QFileInfo(), false);
	}
	else if (saveStarted && !threaded) {
		imageSaved(file);
	}
}

void DkImageLoader::imageSaved(QFileInfo file, bool saved) {

	emit updateSpinnerSignalDelayed(false);
	dirWatcher->blockSignals(false);

	if (!file.exists() || !file.isFile() || !saved)
		return;

	folderUpdated = true;
	loadDir(file);

	emit imageLoadedSignal(currentImage, true);
	emit imageUpdatedSignal(currentImage);
	qDebug() << "image updated: " << currentImage->file().fileName();
}

///**
// * Saves the file (not threaded!).
// * No status information will be displayed if this function is called.
// * @param file the file name/path.
// * @param saveImg the image to be saved.
// **/ 
//void DkImageLoader::saveFileSilentIntern(QFileInfo file, QImage saveImg) {
//
//	QMutexLocker locker(&mutex);
//	
//	this->file.refresh();
//
//	// update watcher
//	if (this->file.exists() && watcher)
//		watcher->removePath(this->file.absoluteFilePath());
//	
//	emit updateInfoSignalDelayed(tr("saving..."), true);
//	QString filePath = file.absoluteFilePath();
//	QImage sImg = (saveImg.isNull()) ? basicLoader.image() : saveImg; 
//	bool saved = basicLoader.save(filePath, sImg);
//	emit updateInfoSignalDelayed(tr("saving..."), false);	// stop the label
//	
//	if (saved)
//		emit updateFileWatcherSignal(file);
//	else 
//		emit updateFileWatcherSignal(this->file);
//
//	if (!saveImg.isNull() && saved) {
//		
//		if (this->file.exists()) {
//			try {
//				// TODO: remove watcher path?!
//				imgMetaData.saveThumbnail(DkThumbsLoader::createThumb(sImg), QFileInfo(filePath));
//				imgMetaData.saveMetaDataToFile(QFileInfo(filePath));
//			} catch (DkException e) {
//
//				qDebug() << "can't write metadata...";
//			} catch (...) {
//				
//				if (!restoreFile(QFileInfo(filePath)))
//					emit newErrorDialog("sorry, I destroyed: " + QFileInfo(filePath).fileName() + "\n remove the numbers after the file extension in order to restore the file...");
//			}
//		}
//
//		// reload my dir (if it was changed...)
//		this->file = QFileInfo(filePath);
//		this->editFile = QFileInfo();
//
//		if (saved)
//			load(filePath, true);
//
//		//this->virtualFile = this->file;
//		//basicLoader.setImage(saveImg, this->file);
//		//loadDir(this->file.absoluteDir());
//
//		//if (cacher) cacher->setCurrentFile(file, basicLoader.image());
//		//sendFileSignal();
//	}
//}

///**
// * Saves the rating to the metadata.
// * This function does nothing if an image format
// * is loaded that does not support metadata.
// * @param rating the rating.
// **/ 
//void DkImageLoader::saveRating(int rating) {
//
//
//
//	file.refresh();
//
//	// file might be edited
//	if (!file.exists())
//		return;
//
//	QMutexLocker locker(&mutex);
//	// update watcher
//	if (this->file.exists() && watcher)
//		watcher->removePath(this->file.absoluteFilePath());
//
//	try {
//		
//		imgMetaData.saveRating(rating);
//	}catch(...) {
//		
//		if (!restoreFile(this->file))
//			emit updateInfoSignal(tr("Sorry, I could not restore: %1").arg(file.fileName()));
//	}
//	emit updateFileWatcher(this->file);
//
//}

//void DkImageLoader::enableWatcher(bool enable) {
//	
//	watcherEnabled = enable;
//}

/**
 * Updates the file history.
 * The file history stores the last 10 folders.
 **/ 
void DkImageLoader::updateHistory() {

	if (!DkSettings::global.logRecentFiles || DkSettings::app.privateMode)
		return;

	if (!currentImage || currentImage->hasImage() != DkImageContainer::loaded || !currentImage->exists())
		return;

	QFileInfo file = currentImage->file();

	// sync with other instances
	QSettings& settings = Settings::instance().getSettings();
	settings.beginGroup("GlobalSettings");
	DkSettings::global.recentFolders = settings.value("recentFolders", DkSettings::global.recentFolders).toStringList();
	DkSettings::global.recentFiles = settings.value("recentFiles", DkSettings::global.recentFiles).toStringList();

	DkSettings::global.lastDir = file.absolutePath();

	DkSettings::global.recentFiles.removeAll(file.absoluteFilePath());
	DkSettings::global.recentFolders.removeAll(file.absolutePath());

	QStringList tmpRecentFiles;

	// try to collect images from different folders
	for (int idx = 0; idx < DkSettings::global.recentFiles.size(); idx++) {
		
		if (DkSettings::global.recentFiles.at(idx).contains(file.absolutePath()))
			tmpRecentFiles.append(DkSettings::global.recentFiles.at(idx));
	}

	if (tmpRecentFiles.size() < qFloor(0.5f*DkSettings::global.numFiles)) {

		// maximum 4 most recent images from the same folder
		for (int idx = tmpRecentFiles.size()-1; idx > 3; idx--) {
			DkSettings::global.recentFiles.removeAll(tmpRecentFiles.at(idx));

		}
	}

	DkSettings::global.recentFiles.push_front(file.absoluteFilePath());
	DkSettings::global.recentFolders.push_front(file.absolutePath());

	DkSettings::global.recentFiles.removeDuplicates();
	DkSettings::global.recentFolders.removeDuplicates();

	for (int idx = 0; idx < DkSettings::global.recentFiles.size()-DkSettings::global.numFiles-10; idx++)
		DkSettings::global.recentFiles.pop_back();

	for (int idx = 0; idx < DkSettings::global.recentFolders.size()-DkSettings::global.numFiles-10; idx++)
		DkSettings::global.recentFolders.pop_back();

	// sync with other instances
	settings.setValue("recentFolders", DkSettings::global.recentFolders);
	settings.setValue("recentFiles", DkSettings::global.recentFiles);
	settings.endGroup();

	//DkSettings s = DkSettings();
	//s.save();
}

// image manipulation --------------------------------------------------------------------
/**
 * Deletes the currently loaded file.
 **/ 
bool DkImageLoader::deleteFile() {
	
	if (currentImage && currentImage->exists()) {

		QFile fileHandle(currentImage->file().absoluteFilePath());

		if (fileHandle.remove()) {
			QSharedPointer<DkImageContainerT> imgC = getSkippedImage(1);
			load(imgC);
			emit showInfoSignal(tr("%1 deleted...").arg(QFileInfo(fileHandle.fileName()).fileName()));
			return true;
		}
		else
			emit showInfoSignal(tr("Sorry, I could not delete: %1").arg(currentImage->file().fileName()));
	}

	return false;
}

/**
 * Rotates the image.
 * First, we try to set the rotation flag in the metadata
 * (this is the fastest way to rotate an image).
 * If this does not work, the image matrix is rotated.
 * @param angle the rotation angle in degree.
 **/ 
void DkImageLoader::rotateImage(double angle) {

	qDebug() << "rotating image...";

	if (!currentImage || !currentImage->hasImage()) {
		qDebug() << "sorry, loader has no image";
		return;
	}

	currentImage->getLoader()->rotate(qRound(angle));

	QImage thumb = DkImage::createThumb(currentImage->image());
	currentImage->getThumb()->setImage(thumb);

	QSharedPointer<DkMetaDataT> metaData = currentImage->getMetaData();
	bool metaDataSet = false;

	if (metaData->hasMetaData() && DkSettings::metaData.saveExifOrientation) {
		try {
			if (!metaData->isJpg())
				metaData->setThumbnail(thumb);
			metaData->setOrientation(qRound(angle));
			metaDataSet = true;
		}
		catch (...) {
		}
	}

	if (!metaDataSet)
		setImage(currentImage->image(), currentImage->file());

	emit imageUpdatedSignal(currentImage);
}

/**
 * Restores files that were destroyed by the Exiv2 lib.
 * If a watch (or some other read lock) is on a file, the
 * Exiv2 lib is known do damage the files on Windows.
 * This function restores these files.
 * @param fileInfo the file to be restored.
 * @return bool true if the file could be restored.
 **/ 
bool DkImageLoader::restoreFile(const QFileInfo& fileInfo) {

	QStringList files = fileInfo.dir().entryList();
	QString fileName = fileInfo.fileName();
	QRegExp filePattern(fileName + "[0-9]+");
	QString backupFileName;

	// if exif crashed it saved a backup file with the format: filename.png1232
	for (int idx = 0; idx < files.size(); idx++) {

		if (filePattern.exactMatch(files[idx])) {
			backupFileName = files[idx];
			break;
		}
	}

	if (backupFileName.isEmpty()) {
		qDebug() << "I could not locate the backup file...";
		return true;
	}

	// delete the destroyed file
	QFile file(fileInfo.absoluteFilePath());
	QFile backupFile(fileInfo.absolutePath() + QDir::separator() + backupFileName);

	if (file.size() == 0 || file.size() <= backupFile.size()) {
		
		if (!file.remove()) {

			// ok I did not destroy the original file - so delete the back-up
			// -> this reverts the file - but otherwise we spam to the disk 
			// actions reverted here include meta data saving
			if (file.size() != 0)
				return backupFile.remove();

			qDebug() << "I could not remove the file...";
			return false;
		}
	}
	else {
		
		qDebug() << "non-empty file: " << fileName << " I won't delete it...";
		qDebug() << "file size: " << file.size() << " back-up file size: " << backupFile.size();
		return false;
	}

	// now 
	return backupFile.rename(fileInfo.absoluteFilePath());
}

/**
 * Reloads the file index if the directory was edited.
 * @param path the path to the current directory
 **/ 
void DkImageLoader::directoryChanged(const QString& path) {

	if (path.isEmpty() || QDir(path) == dir.absolutePath()) {

		folderUpdated = true;
		
		// guarantee, that only every XX seconds a folder update occurs
		// think of a folder where 100s of files are written to...
		// as this could be pretty fast, the thumbsloader (& whoever) would create a 
		// greater offset and slow down the system
		if ((path.isEmpty() && timerBlockedUpdate) || (!path.isEmpty() && !delayedUpdateTimer.isActive())) {

			loadDir(dir, false);
			timerBlockedUpdate = false;

			if (!path.isEmpty())
				delayedUpdateTimer.start(1000);
		}
		else
			timerBlockedUpdate = true;
	}
	
}

/**
 * Returns true if a file was specified.
 * @return bool true if a file name/path was specified
 **/ 
bool DkImageLoader::hasFile() const {

	return currentImage && currentImage->exists();
}

bool DkImageLoader::hasMovie() {

	if (!currentImage || !currentImage->exists())
		return false;

	QString newSuffix = currentImage->file().suffix();
	return newSuffix.contains(QRegExp("(gif|mng)", Qt::CaseInsensitive)) != 0;

}

/**
 * Returns the currently loaded file information.
 * @return QFileInfo the current file info
 **/ 
QFileInfo DkImageLoader::file() const {

	if (!currentImage)
		return QFileInfo();

	
	return currentImage->file();
}

QSharedPointer<DkImageContainerT> DkImageLoader::getCurrentImage() const {

	return currentImage;
}

QSharedPointer<DkImageContainerT> DkImageLoader::getLastImage() const {

	return lastImageLoaded;
}

/**
 * Returns the currently loaded directory.
 * @return QDir the currently loaded directory.
 **/ 
QDir DkImageLoader::getDir() const {

	return dir;
}

QStringList DkImageLoader::getFoldersRecursive(QDir dir) {

	//DkTimer dt;
	QStringList subFolders;
	//qDebug() << "scanning recursively: " << dir.absolutePath();

	if (DkSettings::global.scanSubFolders) {

		QDirIterator dirs(dir.absolutePath(), QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
	
		int nFolders = 0;
		while (dirs.hasNext()) {
			dirs.next();
			subFolders << dirs.filePath();
			nFolders++;

			if (nFolders > 100)
				break;
			
			//getFoldersRecursive(dirs.filePath(), subFolders);
			//qDebug() << "loop: " << dirs.filePath();
		}
	}	

	subFolders << dir.absolutePath();

	qSort(subFolders.begin(), subFolders.end(), DkUtils::compLogicQString);
	

	qDebug() << dir.absolutePath();
	
	//qDebug() << "scanning folders recursively took me: " << QString::fromStdString(dt.getTotal());
	return subFolders;
}

QFileInfoList DkImageLoader::updateSubFolders(QDir rootDir) {
	
	subFolders = getFoldersRecursive(rootDir);
	QFileInfoList files;
	qDebug() << subFolders;

	// find the first subfolder that has images
	for (int idx = 0; idx < subFolders.size(); idx++) {
		dir = subFolders[idx];
		files = getFilteredFileInfoList(dir, ignoreKeywords, keywords);		// this line takes seconds if you have lots of files and slow loading (e.g. network)
		if (!files.empty())
			break;
	}

	return files;
}

int DkImageLoader::getNextFolderIdx(int folderIdx) {
	
	int nextIdx = -1;

	if (subFolders.empty())
		return nextIdx;

	// find the first sub folder that has images
	for (int idx = 1; idx < subFolders.size(); idx++) {
		
		int tmpNextIdx = folderIdx + idx;

		if (DkSettings::global.loop)
			tmpNextIdx %= subFolders.size();
		else if (tmpNextIdx >= subFolders.size())
			return -1;

		QDir cDir = subFolders[tmpNextIdx];
		QFileInfoList cFiles = getFilteredFileInfoList(cDir, ignoreKeywords, keywords);		// this line takes seconds if you have lots of files and slow loading (e.g. network)
		if (!cFiles.empty()) {
			nextIdx = tmpNextIdx;
			break;
		}
	}

	return nextIdx;
}

int DkImageLoader::getPrevFolderIdx(int folderIdx) {
	
	int prevIdx = -1;

	if (subFolders.empty())
		return prevIdx;

	// find the first sub folder that has images
	for (int idx = 1; idx < subFolders.size(); idx++) {

		int tmpPrevIdx = folderIdx - idx;

		if (DkSettings::global.loop && tmpPrevIdx < 0)
			tmpPrevIdx += subFolders.size();
		else if (tmpPrevIdx < 0)
			return -1;

		QDir cDir = subFolders[tmpPrevIdx];
		QFileInfoList cFiles = getFilteredFileInfoList(cDir, ignoreKeywords, keywords);		// this line takes seconds if you have lots of files and slow loading (e.g. network)
		if (!cFiles.empty()) {
			prevIdx = tmpPrevIdx;
			break;
		}
	}

	return prevIdx;
}

void DkImageLoader::errorDialog(const QString& msg) const {

	QMessageBox errorDialog(qApp->activeWindow());
	errorDialog.setWindowTitle(tr("Error"));
	errorDialog.setIcon(QMessageBox::Critical);
	errorDialog.setText(msg);
	errorDialog.show();

	errorDialog.exec();
}

void DkImageLoader::updateCacher(QSharedPointer<DkImageContainerT> imgC) {

	if (!imgC || !DkSettings::resources.cacheMemory)
		return;

	DkTimer dt;

	//// no caching? delete all
	//if (!DkSettings::resources.cacheMemory) {
	//	for (int idx = 0; idx < images.size(); idx++) {
	//		images.at(idx)->clear();
	//	}
	//	return;
	//}

	int cIdx = findFileIdx(imgC->file(), images);
	float mem = 0;

	if (cIdx == -1) {
		qDebug() << "WARNING: image not found for caching!";
		return;
	}

	for (int idx = 0; idx < images.size(); idx++) {

		// clear images if they are edited
		if (idx != cIdx && images.at(idx)->isEdited()) {
			images.at(idx)->clear();
			continue;
		}

		if (idx >= cIdx-1 && idx <= cIdx+DkSettings::resources.maxImagesCached)
			mem += images.at(idx)->getMemoryUsage();
		else {
			images.at(idx)->clear();
			continue;
		}

		// ignore the last and current one
		if (idx == cIdx-1 || idx == cIdx) {
			continue;
		}
		// fully load the next image
		else if (idx == cIdx+1 && mem < DkSettings::resources.cacheMemory && images.at(idx)->getLoadState() == DkImageContainerT::not_loaded) {
			images.at(idx)->loadImageThreaded();
			qDebug() << "[Cacher] " << images.at(idx)->file().absoluteFilePath() << " fully cached...";
		}
		else if (idx > cIdx && idx < cIdx+DkSettings::resources.maxImagesCached-2 && mem < DkSettings::resources.cacheMemory && images.at(idx)->getLoadState() == DkImageContainerT::not_loaded) {
			//dt.getIvl();
			images.at(idx)->fetchFile();		// TODO: crash detected here
			qDebug() << "[Cacher] " << images.at(idx)->file().absoluteFilePath() << " file fetched...";
		}
	}

	qDebug() << "cache with: " << mem << " MB created in: " << dt.getTotal();

}

/**
 * Returns the file list of the directory dir.
 * Note: this function might get slow if lots of files (> 10000) are in the
 * directory or if the directory is in the net.
 * Currently the file list is sorted according to the system specification.
 * @param dir the directory to load the file list from.
 * @param ignoreKeywords if one of these keywords is in the file name, the file will be ignored.
 * @param keywords if one of these keywords is not in the file name, the file will be ignored.
 * @return QStringList all filtered files of the current directory.
 **/ 
QFileInfoList DkImageLoader::getFilteredFileInfoList(const QDir& dir, QStringList ignoreKeywords, QStringList keywords, QStringList folderKeywords) {

	DkTimer dt;

#ifdef WIN32

	QString winPath = QDir::toNativeSeparators(dir.path()) + "\\*.*";

	const wchar_t* fname = reinterpret_cast<const wchar_t *>(winPath.utf16());

	WIN32_FIND_DATAW findFileData;
	HANDLE MyHandle = FindFirstFileW(fname, &findFileData);

	std::vector<std::wstring> fileNameList;
	std::wstring fileName;

	if( MyHandle != INVALID_HANDLE_VALUE) {
		
		do {

			fileName = findFileData.cFileName;
			fileNameList.push_back(fileName);	// TODO: sort correct according to numbers
		} while(FindNextFileW(MyHandle, &findFileData) != 0);
	}

	FindClose(MyHandle);
	
	// slow regexp
	//QString extPattern = ".+((\\" + fileFilters.join("$)|(\\") + "$))";
	//extPattern.replace("*", "");
	//QRegExp exp(extPattern, Qt::CaseInsensitive);

	// remove the * in fileFilters
	QStringList fileFiltersClean = DkSettings::app.browseFilters;
	for (int idx = 0; idx < fileFiltersClean.size(); idx++)
		fileFiltersClean[idx].replace("*", "");

	//std::sort(fileNameList.begin(), fileNameList.end(), wCompLogic);

	QStringList fileList;
	std::vector<std::wstring>::iterator lIter = fileNameList.begin();

	// convert to QStringList
	for (unsigned int idx = 0; idx < fileNameList.size(); idx++, lIter++) {
		
		QString qFilename = DkUtils::stdWStringToQString(*lIter);

		// believe it or not, but this is 10 times faster than QRegExp
		// drawback: we also get files that contain *.jpg*
		for (int idx = 0; idx < fileFiltersClean.size(); idx++) {

			if (qFilename.contains(fileFiltersClean[idx], Qt::CaseInsensitive)) {
				fileList.append(qFilename);
				break;
			}
		}
	}

	qDebug() << "WinAPI, indexed (" << fileList.size() <<") files in: " << dt.getTotal();
#else

	// true file list
	QDir tmpDir = dir;
	tmpDir.setSorting(QDir::LocaleAware);
	QStringList fileList = tmpDir.entryList(DkSettings::app.browseFilters);
	qDebug() << "Qt, sorted file list computed in: " << dt.getIvl();
	qDebug() << fileList;

#endif

	for (int idx = 0; idx < ignoreKeywords.size(); idx++) {
		QRegExp exp = QRegExp("^((?!" + ignoreKeywords[idx] + ").)*$");
		exp.setCaseSensitivity(Qt::CaseInsensitive);
		fileList = fileList.filter(exp);
	}

	for (int idx = 0; idx < keywords.size(); idx++) {
		fileList = fileList.filter(keywords[idx], Qt::CaseInsensitive);
	}

	if (!folderKeywords.empty()) {
		
		QStringList resultList = fileList;
		for (int idx = 0; idx < folderKeywords.size(); idx++) {
			resultList = resultList.filter(folderKeywords[idx], Qt::CaseInsensitive);
		}

		// if string match returns nothing -> try a regexp
		if (resultList.empty())
			resultList = fileList.filter(QRegExp(folderKeywords.join(" ")));

		qDebug() << "filtered file list (get)" << resultList;
		qDebug() << "keywords: " << folderKeywords;
		fileList = resultList;
	}

	if (DkSettings::resources.filterDuplicats) {

		QString preferredExtension = DkSettings::resources.preferredExtension;
		preferredExtension = preferredExtension.replace("*.", "");
		qDebug() << "preferred extension: " << preferredExtension;

		QStringList resultList = fileList;
		fileList.clear();
		
		for (int idx = 0; idx < resultList.size(); idx++) {
			
			QFileInfo cFName = QFileInfo(resultList.at(idx));

			if (preferredExtension.compare(cFName.suffix(), Qt::CaseInsensitive) == 0) {
				fileList.append(resultList.at(idx));
				continue;
			}

			QString cFBase = cFName.baseName();
			bool remove = false;

			for (int cIdx = 0; cIdx < resultList.size(); cIdx++) {

				QString ccBase = QFileInfo(resultList.at(cIdx)).baseName();

				if (cIdx != idx && ccBase == cFBase && resultList.at(cIdx).contains(preferredExtension, Qt::CaseInsensitive)) {
					remove = true;
					break;
				}
			}
			
			if (!remove)
				fileList.append(resultList.at(idx));
		}
	}

	//fileList = sort(fileList, dir);

	QFileInfoList fileInfoList;
	
	for (int idx = 0; idx < fileList.size(); idx++)
		fileInfoList.append(QFileInfo(dir, fileList.at(idx)));

	return fileInfoList;
}

void DkImageLoader::sort() {
	
	qSort(images.begin(), images.end(), imageContainerLessThanPtr);
	emit updateDirSignal(images);
}

/**
 * Returns the directory where files are saved to.
 * @return QDir the directory where the user saved the last file to.
 **/ 
QDir DkImageLoader::getSaveDir() const {

	qDebug() << "save dir: " << dir;

	if (!saveDir.exists())
		return dir;
	else
		return saveDir;
}

/**
* Returns if an image is loaded currently.
* @return bool true if an image is loaded.
**/ 
bool DkImageLoader::hasImage() const {
		
	return currentImage && currentImage->hasImage();
};

bool DkImageLoader::isEdited() const {
	return currentImage && currentImage->isEdited();
};

int DkImageLoader::numFiles() const {
	return images.size();
};

/**
	* Returns the currently loaded image.
	* @return QImage the current image
	**/ 
QImage DkImageLoader::getImage() {
		
	if (!currentImage)
		return QImage();

	return currentImage->image();
};

bool DkImageLoader::dirtyTiff() {

	if (!currentImage)
		return false;

	return currentImage->getLoader()->isDirty();
}

void DkImageLoader::loadLastDir() {

	if (DkSettings::global.recentFolders.empty())
		return;

	QDir lastDir = DkSettings::global.recentFolders[0];
	setDir(lastDir);
}

void DkImageLoader::setFolderFilter(const QString& filter) {

	setFolderFilters(filter.split(" "));
}

void DkImageLoader::setFolderFilters(QStringList filters) {

	folderKeywords = filters;
	folderUpdated = true;
	loadDir(dir);	// simulate a folder update operation

	//if (!filters.empty() && !images.contains(currentImage))
	//	loadFileAt(0);

	emit folderFiltersChanged(folderKeywords);
}

QStringList DkImageLoader::getFolderFilters() {
	return folderKeywords;
}

/**
 * Sets the current directory to dir.
 * @param dir the directory to be loaded.
 **/ 
void DkImageLoader::setDir(QDir& dir) {

	//QDir oldDir = file.absoluteDir();
	
	bool valid = loadDir(dir);

	if (valid)
		firstFile();
}

/**
 * Sets a new save directory.
 * @param dir the new save directory.
 **/ 
void DkImageLoader::setSaveDir(QDir& dir) {
	this->saveDir = dir;
}

/**
 * Sets the current image to img.
 * @param img the loader's new image.
 **/ 
QSharedPointer<DkImageContainerT> DkImageLoader::setImage(QImage img, QFileInfo editFile) {
	
	qDebug() << "edited file: " << editFile.absoluteFilePath();

	QSharedPointer<DkImageContainerT> newImg = findOrCreateFile(editFile);
	newImg->setImage(img, editFile);
	
	setCurrentImage(newImg);
	emit imageUpdatedSignal(currentImage);

	return newImg;
}


QSharedPointer<DkImageContainerT> DkImageLoader::setImage(QSharedPointer<DkImageContainerT> img) {

	setCurrentImage(img);
	emit imageUpdatedSignal(currentImage);

	return img;
}

/**
 * Returns the current file name.
 * @return QString the file name of the currently loaded file.
 **/ 
QString DkImageLoader::fileName() {
	
	if (!currentImage || !currentImage->exists())
		return QString();
	
	return currentImage->file().fileName();
}

// DkColorLoader --------------------------------------------------------------------
DkColorLoader::DkColorLoader(QVector<QSharedPointer<DkImageContainerT> > images) {

	moveToThread(this);
	init();
}

void DkColorLoader::init() {

	isActive = true;
	maxThumbs = 800;
}

void DkColorLoader::run() {

	int updateIvl = 30;

	// max full HD
	for (int idx = 0; idx <= maxThumbs && idx < images.size(); idx++) {

		//mutex.lock();
		if (!isActive) {
			qDebug() << "color loader stopped...";
			//mutex.unlock();
			break;
		}

		loadColor(idx);

		if ((idx % updateIvl) == 0)
			emit updateSignal(cols, indexes);
	}

	emit updateSignal(cols, indexes);

}

const QVector<QColor>& DkColorLoader::getColors() const {
	return cols;
}

const QVector<int>& DkColorLoader::getIndexes() const {
	return indexes;
}

int DkColorLoader::maxFiles() const {
	return maxThumbs;
}

QString DkColorLoader::getFilename(int idx) const {

	if (idx < 0 || idx >= images.size())
		return QString("");

	return images.at(idx)->file().fileName();
}

void DkColorLoader::loadColor(int fileIdx) {
	
	if (images.size() > maxThumbs)
		fileIdx = qRound((float)fileIdx/maxThumbs*(images.size()-1));

	QSharedPointer<DkThumbNailT> thumb = images.at(fileIdx)->getThumb();

	QImage thumbImg;
	if (thumb->hasImage() != DkThumbNailT::loaded)
		thumb->compute(DkThumbNailT::force_exif_thumb);
	thumbImg = thumb->getImage();
		
	if (!thumb.isNull()) {
		cols.append(DkImage::getMeanColor(thumbImg));	// TODO: compute most significant color
		indexes.append(fileIdx);
	}

}

void DkColorLoader::stop() {

	isActive = false;
}

}
