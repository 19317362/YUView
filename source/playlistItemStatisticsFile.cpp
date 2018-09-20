/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "playlistItemStatisticsFile.h"

#include <cassert>
#include <iostream>
#include <QDebug>
#include <QtConcurrent>
#include <QTime>
#include "statisticsExtensions.h"

// The internal buffer for parsing the starting positions. The buffer must not be larger than 2GB
// so that we can address all the positions in it with int (using such a large buffer is not a good
// idea anyways)
#define STAT_PARSING_BUFFER_SIZE 1048576

playlistItemStatisticsFile::playlistItemStatisticsFile(const QString &itemNameOrFileName)
  : playlistItem(itemNameOrFileName, playlistItem_Indexed)
{
  // Set default variables
  fileSortedByPOC = false;
  blockOutsideOfFrame_idx = -1;
  backgroundParserProgress = 0.0;
  currentDrawnFrameIdx = -1;
  maxPOC = 0;
  isStatisticsLoading = false;

  // Set statistics icon
  setIcon(0, convertIcon(":img_stats.png"));

  file.openFile(itemNameOrFileName);
  if (!file.isOk())
    return;

  // Read the statistics file header
  readHeaderFromFile();

  // Run the parsing of the file in the background
  cancelBackgroundParser = false;
  timer.start(1000, this);

  backgroundParserFuture = QtConcurrent::run(this, &playlistItemStatisticsFile::readFrameAndTypePositionsFromFile);

  connect(&statSource, &statisticHandler::updateItem, [this](bool redraw){ emit signalItemChanged(redraw, RECACHE_NONE); });
  connect(&statSource, &statisticHandler::requestStatisticsLoading, this, &playlistItemStatisticsFile::loadStatisticToCache, Qt::DirectConnection);
}

playlistItemStatisticsFile::~playlistItemStatisticsFile()
{
  // The playlistItemStatisticsFile object is being deleted.
  // Check if the background thread is still running.
  if (backgroundParserFuture.isRunning())
  {
    // signal to background thread that we want to cancel the processing
    cancelBackgroundParser = true;
    backgroundParserFuture.waitForFinished();
  }
}

infoData playlistItemStatisticsFile::getInfo() const
{
  infoData info("Statistics File info");

  // Append the file information (path, date created, file size...)
  info.items.append(file.getFileInfoList());

  // Is the file sorted by POC?
  info.items.append(infoItem("Sorted by POC", fileSortedByPOC ? "Yes" : "No"));

  // Show the progress of the background parsing (if running)
  if (backgroundParserFuture.isRunning())
    info.items.append(infoItem("Parsing:", QString("%1%...").arg(backgroundParserProgress, 0, 'f', 2)));

  // Print a warning if one of the blocks in the statistics file is outside of the defined "frame size"
  if (blockOutsideOfFrame_idx != -1)
    info.items.append(infoItem("Warning", QString("A block in frame %1 is outside of the given size of the statistics.").arg(blockOutsideOfFrame_idx)));

  // Show any errors that occurred during parsing
  if (!parsingError.isEmpty())
    info.items.append(infoItem("Parsing Error:", parsingError));

  return info;
}

void playlistItemStatisticsFile::drawItem(QPainter *painter, int frameIdx, double zoomFactor, bool drawRawData)
{
  // drawRawData only controls the drawing of raw pixel values
  Q_UNUSED(drawRawData);
  const int frameIdxInternal = getFrameIdxInternal(frameIdx);

  // Tell the statSource to draw the statistics
  statSource.paintStatistics(painter, frameIdxInternal, zoomFactor);

  // Currently this frame is drawn.
  currentDrawnFrameIdx = frameIdxInternal;
}

/** The background task that parses the file and extracts the exact file positions
* where a new frame or a new type starts. If the user then later requests this type/POC
* we can directly jump there and parse the actual information. This way we don't have to
* scan the whole file which can get very slow for large files.
*
* This function might emit the objectInformationChanged() signal if something went wrong,
* setting the error message, or if parsing finished successfully.
*/
void playlistItemStatisticsFile::readFrameAndTypePositionsFromFile()
{
  try
  {
    // Open the file (again). Since this is a background process, we open the file again to
    // not disturb any reading from not background code.
    fileSource inputFile;
    if (!inputFile.openFile(file.absoluteFilePath()))
      return;

    // We perform reading using an input buffer
    QByteArray inputBuffer;
    bool fileAtEnd = false;
    qint64 bufferStartPos = 0;

    QString lineBuffer;
    qint64  lineBufferStartPos = 0;
    int     lastPOC = INT_INVALID;
    int     lastType = INT_INVALID;
    bool    sortingFixed = false; 
    
    while (!fileAtEnd && !cancelBackgroundParser)
    {
      // Fill the buffer
      int bufferSize = inputFile.readBytes(inputBuffer, bufferStartPos, STAT_PARSING_BUFFER_SIZE);
      if (bufferSize < STAT_PARSING_BUFFER_SIZE)
        // Less bytes than the maximum buffer size were read. The file is at the end.
        // This is the last run of the loop.
        fileAtEnd = true;

      for (int i = 0; i < bufferSize; i++)
      {
        // Search for '\n' newline characters
        if (inputBuffer.at(i) == 10)
        {
          // We found a newline character
          if (lineBuffer.size() > 0)
          {
            // Parse the previous line
            // get components of this line
            QStringList rowItemList = parseCSVLine(lineBuffer, ';');

            // ignore empty entries and headers
            if (!rowItemList[0].isEmpty() && rowItemList[0][0] != '%')
            {
              // check for POC/type information
              int poc = rowItemList[0].toInt();
              int typeID = rowItemList[5].toInt();

              if (lastType == -1 && lastPOC == -1)
              {
                // First POC/type line
                pocTypeStartList[poc][typeID] = lineBufferStartPos;
                if (poc == currentDrawnFrameIdx)
                  // We added a start position for the frame index that is currently drawn. We might have to redraw.
                  emit signalItemChanged(true, RECACHE_NONE);

                lastType = typeID;
                lastPOC = poc;

                // update number of frames
                if (poc > maxPOC)
                  maxPOC = poc;
              }
              else if (typeID != lastType && poc == lastPOC)
              {
                // we found a new type but the POC stayed the same.
                // This seems to be an interleaved file
                // Check if we already collected a start position for this type
                if (!sortingFixed)
                {
                  // we only check the first occurence of this, in a non-interleaved file
                  // the above condition can be met and will reset fileSortedByPOC
                  
                  fileSortedByPOC = true;
                  sortingFixed = true; 
                }
                lastType = typeID;
                if (!pocTypeStartList[poc].contains(typeID))
                {
                  pocTypeStartList[poc][typeID] = lineBufferStartPos;
                  if (poc == currentDrawnFrameIdx)
                    // We added a start position for the frame index that is currently drawn. We might have to redraw.
                    emit signalItemChanged(true, RECACHE_NONE);
                }
              }
              else if (poc != lastPOC)
              {
                // this is apparently not sorted by POCs and we will not check it further
                if(!sortingFixed)
                  sortingFixed = true;
                
                // We found a new POC
                if (fileSortedByPOC)
                {
                  // There must not be a start position for any type with this POC already.
                  if (pocTypeStartList.contains(poc))
                    throw "The data for each POC must be continuous in an interleaved statistics file->";
                }
                else
                {
                
                  // There must not be a start position for this POC/type already.
                  if (pocTypeStartList.contains(poc) && pocTypeStartList[poc].contains(typeID))
                    throw "The data for each typeID must be continuous in an non interleaved statistics file->";
                }

                lastPOC = poc;
                lastType = typeID;

                pocTypeStartList[poc][typeID] = lineBufferStartPos;
                if (poc == currentDrawnFrameIdx)
                  // We added a start position for the frame index that is currently drawn. We might have to redraw.
                  emit signalItemChanged(true, RECACHE_NONE);

                // update number of frames
                if (poc > maxPOC)
                  maxPOC = poc;

                // Update percent of file parsed
                backgroundParserProgress = ((double)lineBufferStartPos * 100 / (double)inputFile.getFileSize());
              }
            }
          }

          lineBuffer.clear();
          lineBufferStartPos = bufferStartPos + i + 1;
        }
        else
          // No newline character found
          lineBuffer.append(inputBuffer.at(i));
      }

      bufferStartPos += bufferSize;
    }

    setStartEndFrame( indexRange(0, maxPOC), false);

    // copy states from one statistics-handler to the other one
    chartStatSource = statSource;

    // Parsing complete
    backgroundParserProgress = 100.0;

    setStartEndFrame( indexRange(0, maxPOC), false );
    emit signalItemChanged(false, RECACHE_NONE);

  } // try
  catch (const char *str)
  {
    std::cerr << "Error while parsing meta data: " << str << '\n';
    parsingError = QString("Error while parsing meta data: ") + QString(str);
    emit signalItemChanged(false, RECACHE_NONE);
    return;
  }
  catch (...)
  {
    std::cerr << "Error while parsing meta data.";
    parsingError = QString("Error while parsing meta data.");
    emit signalItemChanged(false, RECACHE_NONE);
    return;
  }

  return;
}

void playlistItemStatisticsFile::readHeaderFromFile()
{
  try
  {
    if (!file.isOk())
      return;

    // Cleanup old types
    statSource.clearStatTypes();

    // scan header lines first
    // also count the lines per Frame for more efficient memory allocation
    // if an ID is used twice, the data of the first gets overwritten
    bool typeParsingActive = false;
    StatisticsType aType;

    while (!file.atEnd())
    {
      // read one line
      QByteArray aLineByteArray = file.readLine();
      QString aLine(aLineByteArray);

      // get components of this line
      QStringList rowItemList = parseCSVLine(aLine, ';');

      if (rowItemList[0].isEmpty())
        continue;

      // either a new type or a line which is not header finishes the last type
      if (((rowItemList[1] == "type") || (rowItemList[0][0] != '%')) && typeParsingActive)
      {
        // Last type is complete. Store this initial state.
        aType.setInitialState();
        statSource.addStatType(aType);

        // start from scratch for next item
        aType = StatisticsType();
        typeParsingActive = false;

        // if we found a non-header line, stop here
        if (rowItemList[0][0] != '%')
          return;
      }

      if (rowItemList[1] == "type")   // new type
      {
        aType.typeID = rowItemList[2].toInt();
        aType.typeName = rowItemList[3];

        // The next entry (4) is "map", "range", or "vector"
        if(rowItemList.count() >= 5)
        {
          if (rowItemList[4] == "map" || rowItemList[4] == "range")
          {
            aType.hasValueData = true;
            aType.renderValueData = true;
          }
          else if (rowItemList[4] == "vector" || rowItemList[4] == "line")
          {
            aType.hasVectorData = true;
            aType.renderVectorData = true;
            if (rowItemList[4] == "line")
              aType.arrowHead=StatisticsType::arrowHead_t::none;
          }
        }

        typeParsingActive = true;
      }
      else if (rowItemList[1] == "mapColor")
      {
        int id = rowItemList[2].toInt();

        // assign color
        unsigned char r = (unsigned char)rowItemList[3].toInt();
        unsigned char g = (unsigned char)rowItemList[4].toInt();
        unsigned char b = (unsigned char)rowItemList[5].toInt();
        unsigned char a = (unsigned char)rowItemList[6].toInt();

        aType.colMapper.type = colorMapper::mappingType::map;
        aType.colMapper.colorMap.insert(id, QColor(r, g, b, a));
      }
      else if (rowItemList[1] == "range")
      {
        // This is a range with min/max
        int min = rowItemList[2].toInt();
        unsigned char r = (unsigned char)rowItemList[4].toInt();
        unsigned char g = (unsigned char)rowItemList[6].toInt();
        unsigned char b = (unsigned char)rowItemList[8].toInt();
        unsigned char a = (unsigned char)rowItemList[10].toInt();
        QColor minColor = QColor(r, g, b, a);

        int max = rowItemList[3].toInt();
        r = rowItemList[5].toInt();
        g = rowItemList[7].toInt();
        b = rowItemList[9].toInt();
        a = rowItemList[11].toInt();
        QColor maxColor = QColor(r, g, b, a);

        aType.colMapper = colorMapper(min, minColor, max, maxColor);
      }
      else if (rowItemList[1] == "defaultRange")
      {
        // This is a color gradient function
        int min = rowItemList[2].toInt();
        int max = rowItemList[3].toInt();
        QString rangeName = rowItemList[4];

        aType.colMapper = colorMapper(rangeName, min, max);
      }
      else if (rowItemList[1] == "vectorColor")
      {
        unsigned char r = (unsigned char)rowItemList[2].toInt();
        unsigned char g = (unsigned char)rowItemList[3].toInt();
        unsigned char b = (unsigned char)rowItemList[4].toInt();
        unsigned char a = (unsigned char)rowItemList[5].toInt();
        aType.vectorPen.setColor(QColor(r, g, b, a));
      }
      else if (rowItemList[1] == "gridColor")
      {
        unsigned char r = (unsigned char)rowItemList[2].toInt();
        unsigned char g = (unsigned char)rowItemList[3].toInt();
        unsigned char b = (unsigned char)rowItemList[4].toInt();
        unsigned char a = 255;
        aType.gridPen.setColor(QColor(r, g, b, a));
      }
      else if (rowItemList[1] == "scaleFactor")
      {
        aType.vectorScale = rowItemList[2].toInt();
      }
      else if (rowItemList[1] == "scaleToBlockSize")
      {
        aType.scaleValueToBlockSize = (rowItemList[2] == "1");
      }
      else if (rowItemList[1] == "seq-specs")
      {
        QString seqName = rowItemList[2];
        QString layerId = rowItemList[3];
        // For now do nothing with this information.
        // Show the file name for this item instead.
        int width = rowItemList[4].toInt();
        int height = rowItemList[5].toInt();
        if (width > 0 && height > 0)
          statSource.statFrameSize = QSize(width, height);
        if (rowItemList[6].toDouble() > 0.0)
          frameRate = rowItemList[6].toDouble();
      }
    }

  } // try
  catch (const char *str)
  {
    std::cerr << "Error while parsing meta data: " << str << '\n';
    parsingError = QString("Error while parsing meta data: ") + QString(str);
    return;
  }
  catch (...)
  {
    std::cerr << "Error while parsing meta data.";
    parsingError = QString("Error while parsing meta data.");
    return;
  }

  return;
}

void playlistItemStatisticsFile::loadStatisticToCache(int frameIdxInternal, int typeID)
{
  QMutexLocker lock(&this->mLockStatAccess);
  try
  {
    if (!file.isOk())
      return;

    QTextStream in(file.getQFile());

    if (!pocTypeStartList.contains(frameIdxInternal) || !pocTypeStartList[frameIdxInternal].contains(typeID))
    {
      // There are no statistics in the file for the given frame and index.
      statSource.statsCache.insert(typeID, statisticsData());
      chartStatSource.statsCache.insert(typeID, statisticsData());
      return;
    }


    qint64 startPos = pocTypeStartList[frameIdxInternal][typeID];
    if (fileSortedByPOC)
    {
      // If the statistics file is sorted by POC we have to start at the first entry of this POC and parse the
      // file until another POC is encountered. If this is not done, some information from a different typeID
      // could be ignored during parsing.

      // Get the position of the first line with the given frameIdxInternal
      startPos = std::numeric_limits<qint64>::max();
      for (const qint64 &value : pocTypeStartList[frameIdxInternal])
        if (value < startPos)
          startPos = value;
    }

    // fast forward
    in.seek(startPos);

    while (!in.atEnd())
    {
      // read one line
      QString aLine = in.readLine();

      // get components of this line
      QStringList rowItemList = parseCSVLine(aLine, ';');

      if (rowItemList[0].isEmpty())
        continue;

      int poc = rowItemList[0].toInt();
      int type = rowItemList[5].toInt();

      // if there is a new POC, we are done here!
      if (poc != frameIdxInternal)
        break;
      // if there is a new type and this is a non interleaved file, we are done here.
      if (!fileSortedByPOC && type != typeID)
        break;

      int values[4] = {0};

      values[0] = rowItemList[6].toInt();

      bool vectorData = false;
      bool lineData = false; // or a vector specified by 2 points

      if (rowItemList.count() > 7)
      {
        values[1] = rowItemList[7].toInt();
        vectorData = true;
      }
      if (rowItemList.count() > 8)
      {
        values[2] = rowItemList[8].toInt();
        values[3] = rowItemList[9].toInt();
        lineData = true;
        vectorData = false;
      }

      int posX = rowItemList[1].toInt();
      int posY = rowItemList[2].toInt();
      int width = rowItemList[3].toUInt();
      int height = rowItemList[4].toUInt();

      // Check if block is within the image range
      if (blockOutsideOfFrame_idx == -1 && (posX + width > statSource.statFrameSize.width() || posY + height > statSource.statFrameSize.height()))
        // Block not in image. Warn about this.
        blockOutsideOfFrame_idx = frameIdxInternal;

      const StatisticsType *statsType = statSource.getStatisticsType(type);
      Q_ASSERT_X(statsType != nullptr, "StatisticsObject::readStatisticsFromFile", "Stat type not found.");

      if (vectorData && statsType->hasVectorData)
      {
        statSource.statsCache[type].addBlockVector(posX, posY, width, height, values[0], values[1]);
        chartStatSource.statsCache[type].addBlockVector(posX, posY, width, height, values[0], values[1]);
      }
      else if (lineData && statsType->hasVectorData)
      {
        statSource.statsCache[type].addLine(posX, posY, width, height, values[0], values[1], values[2], values[3]);
        chartStatSource.statsCache[type].addLine(posX, posY, width, height, values[0], values[1], values[2], values[3]);
      }
      else
      {
        statSource.statsCache[type].addBlockValue(posX, posY, width, height, values[0]);
        chartStatSource.statsCache[type].addBlockValue(posX, posY, width, height, values[0]);
      }
    }

  } // try
  catch (const char *str)
  {
    std::cerr << "Error while parsing: " << str << '\n';
    parsingError = QString("Error while parsing meta data: ") + QString(str);
    return;
  }
  catch (...)
  {
    std::cerr << "Error while parsing.";
    parsingError = QString("Error while parsing meta data.");
    return;
  }

  return;
}

QStringList playlistItemStatisticsFile::parseCSVLine(const QString &srcLine, char delimiter) const
{
  // first, trim newline and white spaces from both ends of line
  QString line = srcLine.trimmed().remove(' ');

  // now split string with delimiter
  return line.split(delimiter);
}

// This timer event is called regularly when the background loading process is running.
// It will update
void playlistItemStatisticsFile::timerEvent(QTimerEvent *event)
{
  if (event->timerId() != timer.timerId())
    return playlistItem::timerEvent(event);

  // Check if the background process is still running. If it is not, no signal are required anymore.
  // The final update signal was emitted by the background process.
  if (!backgroundParserFuture.isRunning())
    timer.stop();
  else
  {
    setStartEndFrame(indexRange(0, maxPOC), false);
    emit signalItemChanged(false, RECACHE_NONE);
  }
}

void playlistItemStatisticsFile::createPropertiesWidget()
{
  // Absolutely always only call this once//
  assert(!propertiesWidget);

  // Create a new widget and populate it with controls
  preparePropertiesWidget(QStringLiteral("playlistItemStatisticsFile"));

  // On the top level everything is layout vertically
  QVBoxLayout *vAllLaout = new QVBoxLayout(propertiesWidget.data());

  QFrame *line = new QFrame;
  line->setObjectName(QStringLiteral("lineOne"));
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);

  vAllLaout->addLayout(createPlaylistItemControls());
  vAllLaout->addWidget(line);
  vAllLaout->addLayout(statSource.createStatisticsHandlerControls());

  // Do not add any stretchers at the bottom because the statistics handler controls will
  // expand to take up as much space as there is available
}

void playlistItemStatisticsFile::savePlaylist(QDomElement &root, const QDir &playlistDir) const
{
  // Determine the relative path to the YUV file-> We save both in the playlist.
  QUrl fileURL(file.getAbsoluteFilePath());
  fileURL.setScheme("file");
  QString relativePath = playlistDir.relativeFilePath(file.getAbsoluteFilePath());

  QDomElementYUView d = root.ownerDocument().createElement("playlistItemStatisticsFile");

  // Append the properties of the playlistItem
  playlistItem::appendPropertiesToPlaylist(d);

  // Append all the properties of the YUV file (the path to the file-> Relative and absolute)
  d.appendProperiteChild("absolutePath", fileURL.toString());
  d.appendProperiteChild("relativePath", relativePath);

  // Save the status of the statistics (which are shown, transparency ...)
  statSource.savePlaylist(d);

  root.appendChild(d);
}

playlistItemStatisticsFile *playlistItemStatisticsFile::newplaylistItemStatisticsFile(const QDomElementYUView &root, const QString &playlistFilePath)
{
  // Parse the DOM element. It should have all values of a playlistItemStatisticsFile
  QString absolutePath = root.findChildValue("absolutePath");
  QString relativePath = root.findChildValue("relativePath");

  // check if file with absolute path exists, otherwise check relative path
  QString filePath = fileSource::getAbsPathFromAbsAndRel(playlistFilePath, absolutePath, relativePath);
  if (filePath.isEmpty())
    return nullptr;

  // We can still not be sure that the file really exists, but we gave our best to try to find it.
  playlistItemStatisticsFile *newStat = new playlistItemStatisticsFile(filePath);

  // Load the propertied of the playlistItem
  playlistItem::loadPropertiesFromPlaylist(root, newStat);

  // Load the status of the statistics (which are shown, transparency ...)
  newStat->statSource.loadPlaylist(root);

  return newStat;
}

void playlistItemStatisticsFile::getSupportedFileExtensions(QStringList &allExtensions, QStringList &filters)
{
  allExtensions.append("csv");
  filters.append("Statistics File (*.csv)");
}

void playlistItemStatisticsFile::reloadItemSource()
{
  // Set default variables
  fileSortedByPOC = false;
  blockOutsideOfFrame_idx = -1;
  backgroundParserProgress = 0.0;
  parsingError.clear();
  currentDrawnFrameIdx = -1;
  maxPOC = 0;

  // Is the background parser still running? If yes, abort it.
  if (backgroundParserFuture.isRunning())
  {
    // signal to background thread that we want to cancel the processing
    cancelBackgroundParser = true;
    backgroundParserFuture.waitForFinished();
  }

  // Clear the parsed data
  pocTypeStartList.clear();
  statSource.statsCache.clear();
  statSource.statsCacheFrameIdx = -1;

  // Reopen the file
  file.openFile(plItemNameOrFileName);
  if (!file.isOk())
    return;

  // Read the new statistics file header
  readHeaderFromFile();

  statSource.updateStatisticsHandlerControls();

  // Run the parsing of the file in the background
  cancelBackgroundParser = false;
  timer.start(1000, this);
  backgroundParserFuture = QtConcurrent::run(this, &playlistItemStatisticsFile::readFrameAndTypePositionsFromFile);
}

void playlistItemStatisticsFile::loadFrame(int frameIdx, bool playback, bool loadRawdata, bool emitSignals)
{
  Q_UNUSED(playback);
  Q_UNUSED(loadRawdata);
  const int frameIdxInternal = getFrameIdxInternal(frameIdx);

  if (statSource.needsLoading(frameIdxInternal) == LoadingNeeded)
  {
    isStatisticsLoading = true;
    statSource.loadStatistics(frameIdxInternal);
    isStatisticsLoading = false;
    if (emitSignals)
      emit signalItemChanged(true, RECACHE_NONE);
  }
}

QMap<QString, QList<QList<QVariant>>>* playlistItemStatisticsFile::getData(indexRange aRange, bool aReset, QString aType)
{   
  // getting the max range
  indexRange realRange = this->getFrameIdxRange();

  int rangeSize = aRange.second - aRange.first;
  int frameSize = realRange.second - realRange.first;

  if(aReset || (rangeSize != frameSize))
    this->mStatisticData.clear();

  // running through the statisticsList
  foreach (StatisticsType statType, this->chartStatSource.getStatisticsTypeList())
  {
    if(aType == "" || aType == statType.typeName)
    {
      // creating the resultList, where we save all the datalists
      QList<QList<QVariant>> resultList;
      // getting the key
      QString key  = statType.typeName;
      // creating the data list
      QList<QVariant> dataList;

      // getting all the statistic-data by the typeId
      int typeIdx = statType.typeID;

      if (this->isRangeInside(realRange, aRange))
      {
        for(int frame = aRange.first; frame <= aRange.second; frame++)
        {
          dataList.clear();

          // first we have to load the statistic
          this->loadStatisticToCache(frame, typeIdx);

          statisticsData statDataByType = this->chartStatSource.statsCache[typeIdx];
          // the data can be a value or a vector, converting the data into an QVariant and append it to the dataList
          if(statType.hasValueData)
          {
            foreach (statisticsItem_Value val, statDataByType.valueData)
            {
              QVariant variant = QVariant::fromValue(val);
              dataList.append(variant);
            }
          }
          else if(statType.hasVectorData)
          {
            foreach (statisticsItem_Vector val, statDataByType.vectorData)
            {
              QVariant variant = QVariant::fromValue(val);
              dataList.append(variant);
            }
          }
          // appending the data to the resultList
          resultList.append(dataList);
        }
        // adding each key with the resultList, inside of the resultList
        this->mStatisticData.insert(key, resultList);
      }
      if(aType != "")
        break;
    }
  }
  return &this->mStatisticData;
}

bool playlistItemStatisticsFile::isRangeInside(indexRange aOriginalRange, indexRange aCheckRange)
{
  bool wrongDimensionsOriginalRange   = aOriginalRange.first > aOriginalRange.second;
  bool wrongDimensionsCheckRange      = aCheckRange.first > aCheckRange.second;
  bool firstdimension                 = aOriginalRange.first <= aCheckRange.first;
  bool seconddimension                = aOriginalRange.second >= aCheckRange.second;

  return wrongDimensionsOriginalRange || wrongDimensionsCheckRange || (firstdimension && seconddimension);
}

QList<collectedData>* playlistItemStatisticsFile::sortAndCategorizeData(const QString aType, const int aFrameIndex)
{
  auto getSmallestKey = [] (QList<QString> aMapKeys) -> QString {
    int smallestFoundNumber = INT_MAX;
    QString numberString = "";
    QString resultKey = ""; // just a holder

    // getting the smallest number and the label
    foreach (QString label, aMapKeys)
    {
      if(numberString != "") // the if is necessary, otherwise it will crash on windows
        numberString.clear(); // cleaning the String

      for (int run = 0; run < label.length(); run++)
      {
        if(label[run] != 'x') // finding the number befor the 'x'
         numberString.append(label[run]); // creating the number from the chars
        else // we have found the 'x' so the number is finished
          break;
      }

      int number = numberString.toInt(); // convert to int

      // check if we have found the smallest number
      if(number < smallestFoundNumber)
      {
        // found a smaller number so hold it
        smallestFoundNumber = number;
        // we hold the label, so we dont need to "create / build" the key again
        resultKey = label;
      }
    }
    return resultKey;
  };

  //prepare the result
  QMap<QString, QMap<int, int*>*>* dataMapStatisticsItemValue = new QMap<QString, QMap<int, int*>*>;
  QMap<QString, QHash<QPoint, int*>*>* dataMapStatisticsItemVector = new QMap<QString, QHash<QPoint, int*>*>;

  indexRange range(aFrameIndex, aFrameIndex);
  this->getData(range, true, aType);

  // getting allData from the type
  QList<QList<QVariant>> allData = this->mStatisticData.value(aType);

  // getting the data depends on the actual selected frameIndex / POC
  QList<QVariant> data = allData.at(0);

  // now we go thru all elements of the frame
  foreach (QVariant item, data)
  {
    // item-value was defined by statisticsItem_Value @see statisticsExtensions.h
    if(item.canConvert<statisticsItem_Value>())
    {
      statisticsItem_Value value = item.value<statisticsItem_Value>();
      // creating the label: widht x height
      QString label = QString::number(value.size[0]) + "x" + QString::number(value.size[1]);

      int* chartDepthCnt;

      // hard part of the function
      // 1. check if label is in map
      // 2. if not: insert label and a new / second Map with the new values for depth
      // 3. if it was inside: check if Depth was inside the second map
      // 4. if not in second map create new Depth-data-container, fill with data and add to second map
      // 5. if it was in second map just increment the Depth-Counter
      if(!dataMapStatisticsItemValue->contains(label))
      {
        // label was not inside
        QMap<int, int*>* map = new QMap<int, int*>();

        // create Data, set to 0 and increment (or set count to the value 1, same as set to 0 and increment) and add to second map
        chartDepthCnt = new int[2];

        chartDepthCnt[0] = value.value;
        chartDepthCnt[1] = 1;

        map->insert(chartDepthCnt[0], chartDepthCnt);
        dataMapStatisticsItemValue->insert(label, map);
      }
      else
      {
        // label was inside, check if Depth-value is inside
        QMap<int, int*>* map = dataMapStatisticsItemValue->value(label);

        // Depth-Value not inside
        if(!(map->contains(value.value)))
        {
          chartDepthCnt = new int[2];                   // creating new result
          chartDepthCnt[0] = value.value;               // holding the value
          chartDepthCnt[1] = 0;                         // initialise counter to 0
          chartDepthCnt[1]++;                           // increment the counter
          map->insert(chartDepthCnt[0], chartDepthCnt); // at least add to list
        }
        else  // Depth-Value was inside
        {
          // finding the result, every item "value" is just one time in the list
          int* counter = map->value(value.value);
          counter[1]++; // increment the counter
        }
      }
    }

    // same procedure as statisticsItem_Value but at some points it is different
    // in case of statisticsItem_Vector
    if(item.canConvert<statisticsItem_Vector>())
    {
      statisticsItem_Vector vector = item.value<statisticsItem_Vector>();
      QPoint value = vector.point[0];
      short width = vector.size[0];
      short height = vector.size[1];

      QString label = QString::number(width) + "x" + QString::number(height);
      int* chartValueCnt;

      if(!dataMapStatisticsItemVector->contains(label))
      {
        // label was not inside
        QHash<QPoint, int*>* map = new QHash<QPoint, int*>();

        // create Data, set to 0 and increment (or set count to the value 1, same as set to 0 and increment) and add to second map
        chartValueCnt = new int[1];

        chartValueCnt[0] = 1;

        map->insert(value, chartValueCnt);
        dataMapStatisticsItemVector->insert(label, map);
      }
      else
      {
        // label was inside, check if Point-value is inside
        QHash<QPoint, int*>* map = dataMapStatisticsItemVector->value(label);

        // Depth-Value not inside
        if(!(map->contains(value)))
        {
          chartValueCnt = new int[1];                   // creating new result
          chartValueCnt[0] = 0;                         // initialise counter to 0
          chartValueCnt[0]++;                           // increment the counter
          map->insert(value, chartValueCnt); // at least add to list
        }
        else  // Point-Value was inside
        {
          // finding the result, every item "value" is just one time in the list
          int* counter = map->value(value);
          counter[0]++; // increment the counter
        }
      }
    }
  }
  // at least we order the data based on the width & height (from low to high) and make the data handling easier
  QList<collectedData>* resultData = new QList<collectedData>;

  // first init maxElemtents with the amount of data in dataMapStatisticsItemValue
  int maxElementsToNeed = dataMapStatisticsItemValue->keys().count();

  // if we have the type statisticsItem_Value   -- no if, the brackets should improve reading the code
  {
    while(resultData->count() < maxElementsToNeed)
    {
      QString key = getSmallestKey(dataMapStatisticsItemValue->keys());

      // getting the data depends on the "smallest" key
      QMap<int, int*>* map = dataMapStatisticsItemValue->value(key);

      collectedData data;   // creating the data
      data.mStatDataType = sdtStructStatisticsItem_Value;
      data.mLabel = key;    // setting the label

      // copy each data into the list
      foreach (int value, map->keys())
      {
        int* valueAmount = map->value(value);
        data.addValue(QVariant::fromValue(valueAmount[0]), valueAmount[1]);
      }

      // appending the collectedData to the result
      resultData->append(data);

      // reset settings to find
      dataMapStatisticsItemValue->remove(key);
      key.clear();
    }

    // we can delete the dataMap, cause we dont need anymore
    foreach (QString key, dataMapStatisticsItemValue->keys())
    {
      QMap<int, int*>* valuesmap = dataMapStatisticsItemValue->value(key);
      foreach (int valuekey, valuesmap->keys())
      {
        delete valuesmap->value(valuekey);
      }
      delete valuesmap;
    }
    delete dataMapStatisticsItemValue;
  }

  // if we have the type statisticsItem_Value
  maxElementsToNeed += dataMapStatisticsItemVector->keys().count(); // calculate the maximum new, because we add the second map

  { // same as above, the brackets should improve reading the code
    // do ordering and  convert to the struct
    while(resultData->count() < maxElementsToNeed)
    {
      QString key = getSmallestKey(dataMapStatisticsItemVector->keys());

      // getting the data dataMapStatisticsItemVector on the "smallest" key
      auto map = dataMapStatisticsItemVector->value(key);

      collectedData data;   // creating the data
      data.mStatDataType = sdtStructStatisticsItem_Vector;
      data.mLabel = key;    // setting the label

      // copy each data into the list
      foreach (QPoint value, map->keys())
        data.addValue(value, *(map->value(value)));

      // appending the collectedData to the result
      resultData->append(data);

      // reset settings to find
      dataMapStatisticsItemVector->remove(key);
      key.clear();
    }

    // we can delete the dataMap, cause we dont need anymore
    foreach (auto key, dataMapStatisticsItemVector->keys())
    {
      auto valuesmap = dataMapStatisticsItemVector->value(key);
      foreach (auto valuekey, valuesmap->keys())
      {
        delete valuesmap->value(valuekey);
      }
      delete valuesmap;
    }
    delete dataMapStatisticsItemVector;

  }


//  // a debug output
//  for(int i = 0; i< resultData->count(); i++) {
//    collectedData cd = resultData->at(i);
//    foreach (int* valuePair, cd.mValueList) {
//      QString debugstring(cd.mLabel + ": " + QString::number(valuePair[0]) + " : " + QString::number(valuePair[1]));
//      qDebug() << debugstring;
//    }
//  }

  return resultData;
}

QList<collectedData>* playlistItemStatisticsFile::sortAndCategorizeDataByRange(const QString aType, const indexRange aRange)
{
  this->chartStatSource.statsCache.clear();

  //if we have the same frame --> just one frame we look at
  if(aRange.first == aRange.second) // same frame --> just one frame same as current frame
    return this->sortAndCategorizeData(aType, aRange.first);

  // we create a tempory list, to collect all data from all frames
  // and we start to sort them by the label
  QList<collectedData*>* preResult = new QList<collectedData*>();

  // next step get the data for each frame
  for (int frame = aRange.first; frame <= aRange.second; frame++)
  {
    // get the data for the actual frame
    QList<collectedData>* collectedDataByFrameList = this->sortAndCategorizeData(aType, frame);

    // now we have to integrate the new Data from one Frame to all other frames
    for(int i = 0; i< collectedDataByFrameList->count(); i++)
    {
      collectedData frameData = collectedDataByFrameList->at(i);

      bool wasnotinside = true;

      // first: check if we have the collected data-label inside of our result list
      for(int j = 0; j < preResult->count(); j++)
      {
        collectedData* resultCollectedData = preResult->at(j);

        if(*resultCollectedData == frameData)
        {
          resultCollectedData->addValues(frameData);
          wasnotinside = false;
          break;
        }
      }

      // second: the data-label was not inside, so we create and fill with data
      if(wasnotinside)
      {
        collectedData* resultCollectedData = new collectedData;
        resultCollectedData->mLabel = frameData.mLabel;
        resultCollectedData->mStatDataType =frameData.mStatDataType;
        resultCollectedData->addValues(frameData);
        preResult->append(resultCollectedData);
      }
    }
  }

  // at this point we have a tree-structure, each label has a list with all values, but the values are not summed up
  // and we have to

  // we create the data for the result
  QList<collectedData>* result = new QList<collectedData>();

  // running thru all preResult-Elements
  for (int i = 0; i < preResult->count(); i++)
  {
    // creating a list for all Data
    QList<QPair<QVariant, int>*>* tmpDataList = new QList<QPair<QVariant, int>*>();

    //get the data from preResult at an index
    collectedData* preData = preResult->at(i);

    // now we go thru all possible data-elements
    for (int j = 0; j < preData->mValues.count(); j++)
    {
      // getting the real-data (value and amount)
      QPair<QVariant, int>* preDataValuePair = preData->mValues.at(j);

      //define a auxillary-variable
      bool wasnotinside = true;

      // run thru all data, we have already in our list
      for (int k = 0; k < tmpDataList->count(); k++)
      {
        // getting data from our list
        QPair<QVariant, int>* resultData = tmpDataList->at(k);

        // and compare each value for the result with the given value
        if(resultData->first == preDataValuePair->first)
        {
          // if we found an equal pair of value, we have to sum up the amount
          resultData->second += preDataValuePair->second;
          wasnotinside = false;   // take care, that we change our bool
          break; // we can leave the loop, because every value is just one time in our list
        }
      }

      // we have the data not inside our list
      if(wasnotinside)
      {
        // we create a copy and insert it to the list
        QPair<QVariant, int>* dptcnt = new QPair<QVariant, int>();
        dptcnt->first = preDataValuePair->first;
        dptcnt->second = preDataValuePair->second;
        tmpDataList->append(dptcnt);
      }
    }

    //define the new data for the result
    collectedData data;
    data.mLabel = preData->mLabel;
    data.mStatDataType  = preData->mStatDataType;
    data.addValueList(tmpDataList);

    // at least append the new collected Data to our result-list
    result->append(data);
  }

  // we don't need the temporary created preResult anymore (remember: get memory, free memory)
  preResult->clear();
  delete preResult;

  // finally return our result
  return result;
}

bool playlistItemStatisticsFile::isDataAvaible()
{
  return backgroundParserFuture.isFinished();
}
