/*
 * Copyright (c) 2007 Sean C. Rhea (srhea@srhea.net)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

//
// Decodes .srm files.  Adapted from (buggy) description by Stephan Mantler
// (http://www.stephanmantler.com/?page_id=86).
//

#include "SrmRideFile.h"
#include <QDataStream>
#include <QDate>
#include <QFile>
#include <QStringList>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#ifdef WIN32
    #include <winsock.h>
#else
    #include <arpa/inet.h>
#endif

#define PI M_PI

static quint8 readByte(QDataStream &in)
{
    quint8 value;
    in >> value;
    return value;
}

static quint16 readShort(QDataStream &in)
{
    quint16 value;
    in >> value;
    return htons(value); // SRM uses big endian
}

static quint32 readLong(QDataStream &in)
{
    quint32 value;
    in >> value;
    return htonl(value); // SRM uses big endian
}

struct marker
{
    int start, end;
};

struct blockhdr
{
    QDateTime dt;
    quint16 chunkcnt;
};

static int srmFileReaderRegistered =
    RideFileFactory::instance().registerReader(
        "srm", "SRM training files", new SrmFileReader());

RideFile *SrmFileReader::openRideFile(QFile &file, QStringList &errorStrings) const
{
    if (!file.open(QFile::ReadOnly)) {
        errorStrings << QString("can't open file %1").arg(file.fileName());
        return NULL;
    }
    QDataStream in(&file);
    RideFile *result = new RideFile;
    result->setDeviceType("SRM");

    char magic[4];
    in.readRawData(magic, sizeof(magic));
    assert(strncmp(magic, "SRM", 3) == 0);
    int version = magic[3] - '0';
    assert(version == 6 || version == 7);

    quint16 dayssince1880 = readShort(in);
    quint16 wheelcirc = readShort(in);
    quint8 recint1 = readByte(in);
    quint8 recint2 = readByte(in);
    quint16 blockcnt = readShort(in);
    quint16 markercnt = readShort(in);
    readByte(in); // padding
    quint8 commentlen = readByte(in);

    char comment[71];
    in.readRawData(comment, sizeof(comment) - 1);
    comment[commentlen - 1] = '\0';

    result->setRecIntSecs(((double) recint1) / recint2);
    unsigned recintms = (unsigned) round(result->recIntSecs() * 1000.0);

    QDate date(1880, 1, 1);
    date = date.addDays(dayssince1880);

    QVector<marker> markers(markercnt + 1);
    for (int i = 0; i <= markercnt; ++i) {
        char mcomment[256];
        in.readRawData(mcomment, sizeof(mcomment) - 1);
        mcomment[sizeof(mcomment) - 1] = '\0';

        quint8 active = readByte(in);
        quint16 start = readShort(in);
        quint16 end = readShort(in);
        quint16 avgwatts = readShort(in);
        quint16 avghr = readShort(in);
        quint16 avgcad = readShort(in);
        quint16 avgspeed = readShort(in);
        quint16 pwc150 = readShort(in);

	// data fixup: Although the data chunk index in srm files starts
	// with 1, some srmwin wrote files referencing index 0.
	if( end < 1 ) end = 1;
	if( start < 1 ) start = 1;

	// data fixup: some srmwin versions wrote markers with start > end
	if( end < start ){
		markers[i].start = end;
		markers[i].end = start;
	} else {
		markers[i].start = start;
		markers[i].end = end;
	}

        (void) active;
        (void) avgwatts;
        (void) avghr;
        (void) avgcad;
        (void) avgspeed;
        (void) pwc150;
        (void) wheelcirc;
    }

    blockhdr *blockhdrs = new blockhdr[blockcnt];
    for (int i = 0; i < blockcnt; ++i) {
        // In the .srm files generated by Rainer Clasen's srmcmd,
        // hsecsincemidn is a *signed* 32-bit integer.  I haven't seen a
        // negative value in any .srm files generated by srmwin.exe, but
        // since the number of hundredths of a second in a day is << 2^31,
        // it seems safe to always treat this number as signed.
        qint32 hsecsincemidn = readLong(in);
        blockhdrs[i].chunkcnt = readShort(in);
        blockhdrs[i].dt = QDateTime(date);
        blockhdrs[i].dt = blockhdrs[i].dt.addMSecs(hsecsincemidn * 10);
    }

    quint16 zero = readShort(in);
    quint16 slope = readShort(in);
    quint16 datacnt = readShort(in);
    readByte(in); // padding

    (void) zero;
    (void) slope;

    assert(blockcnt > 0);

    int blknum = 0, blkidx = 0, mrknum = 0, interval = 0;
    double km = 0.0, secs = 0.0;

    if (markercnt > 0)
        mrknum = 1;

    for (int i = 0; i < datacnt; ++i) {
        int cad, hr, watts;
        double kph, alt;
        if (version == 6) {
            quint8 ps[3];
            in.readRawData((char*) ps, sizeof(ps));
            cad = readByte(in);
            hr = readByte(in);
            kph = (((((unsigned) ps[1]) & 0xf0) << 3)
                   | (ps[0] & 0x7f)) * 3.0 / 26.0;
            watts = (ps[1] & 0x0f) | (ps[2] << 0x4);
            alt = 0.0;
        }
        else {
            assert(version == 7);
            watts = readShort(in);
            cad = readByte(in);
            hr = readByte(in);
            kph = readLong(in) * 3.6 / 1000.0;
            alt = readLong(in);
            double temp = 0.1 * (qint16) readShort(in);
            (void) temp; // unused for now
        }

        if (i == 0) {
            result->setStartTime(blockhdrs[blknum].dt);
        }
        if (mrknum < markers.size() && i == markers[mrknum].end) {
            ++interval;
            ++mrknum;
        }

        // markers count from 1
        if ((i > 0) && (mrknum < markers.size()) && (i == markers[mrknum].start - 1))
            ++interval;

        km += result->recIntSecs() * kph / 3600.0;

        double nm = watts / 2.0 / PI / cad * 60.0;
        result->appendPoint(secs, cad, hr, km, kph, nm, watts, alt, 0.0, 0.0, 0.0, interval);

        ++blkidx;
        if ((blkidx == blockhdrs[blknum].chunkcnt) && (blknum + 1 < blockcnt)) {
            QDateTime end = blockhdrs[blknum].dt.addMSecs(
                recintms * blockhdrs[blknum].chunkcnt);
            ++blknum;
            blkidx = 0;
            QDateTime start = blockhdrs[blknum].dt;
            qint64 endms =
                ((qint64) end.toTime_t()) * 1000 + end.time().msec();
            qint64 startms =
                ((qint64) start.toTime_t()) * 1000 + start.time().msec();
            double diff_secs = (startms - endms) / 1000.0;
            if (diff_secs < result->recIntSecs()) {
                errorStrings << QString("ERROR: time goes backwards by %1 s"
                                        " on trans " "to block %2"
                                        ).arg(diff_secs).arg(blknum);
                secs += result->recIntSecs(); // for lack of a better option
            }
            else {
                secs += diff_secs;
            }
        }
        else {
            secs += result->recIntSecs();
        }
    }

    double last = 0.0;
    for (int i = 1; i < markers.size(); ++i) {
        const marker &marker = markers[i];
        int start = marker.start - 1;
        double start_secs = result->dataPoints()[start]->secs;
        int end = qMin(marker.end - 1, result->dataPoints().size() - 1);
        double end_secs = result->dataPoints()[end]->secs + result->recIntSecs();
        result->addInterval(last, start_secs, "");
        result->addInterval(start_secs, end_secs, QString("%1").arg(i));
        last = end_secs;
    }
    if (!markers.empty() && markers.last().end < result->dataPoints().size()) {
        double start_secs = result->dataPoints().last()->secs + result->recIntSecs();
        result->addInterval(last, start_secs, "");
    }

    file.close();
    return result;
}

