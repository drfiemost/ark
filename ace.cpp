/*

  ark -- archiver for the KDE project

  Copyright (C) 2005 Henrique Pinto <henrique.pinto@kdemail.net>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include "ace.h"
#include "arkwidget.h"
#include "settings.h"

#include <QDir>

#include <KGlobal>
#include <KLocale>
#include <KDebug>
#include <KUrl>
#include <KMessageBox>
#include <K3Process>
#include <KStandardDirs>

AceArch::AceArch( ArkWidget *gui, const QString &filename )
	: Arch( gui, filename )
{
	//m_archiver_program = m_unarchiver_program = "/usr/local/bin/unace";
	m_archiver_program = m_unarchiver_program = "/home/henrique/ArkTest/teste.sh";
	verifyUtilityIsAvailable( m_archiver_program );

	m_headerString = "Date    Time Packed     Size     RatioFile";

	m_repairYear = 5; m_fixMonth = 6; m_fixDay = 7; m_fixTime = 8;
	m_dateCol = 3;
	m_numCols = 5;

	m_archCols.append( ArchColumns( 7, QRegExp( "[0-3][0-9]" ), 2 ) ); // Day
	m_archCols.append( ArchColumns( 6, QRegExp( "[01][0-9]" ), 2 ) ); // Month
	m_archCols.append( ArchColumns( 5, QRegExp( "[0-9][0-9]" ), 4 ) ); // Year
	m_archCols.append( ArchColumns( 8, QRegExp( "[0-9:]+" ), 8 ) ); // Time
	m_archCols.append( ArchColumns( 2, QRegExp( "[0-9]+" ) ) ); // Compressed Size
	m_archCols.append( ArchColumns( 1, QRegExp( "[0-9]+" ) ) ); // Size
	m_archCols.append( ArchColumns( 9, QRegExp( "[0-9][0-9]%" ) ) ); // Ratio
	m_archCols.append( ArchColumns( 0, QRegExp( "[^\\n]+" ), 4096 ) ); // Name
}

AceArch::~AceArch()
{
}

void AceArch::setHeaders()
{
	ColumnList list;
	list.append( FILENAME_COLUMN  );
	list.append( SIZE_COLUMN      );
	list.append( PACKED_COLUMN    );
	list.append( TIMESTAMP_COLUMN );

	emit headers( list );
}

void AceArch::open()
{
	kDebug(1601) << "+AceArch::open()" << endl;
	setHeaders();

	m_buffer = "";
	m_header_removed = false;
	m_finished = false;

	K3Process *kp = m_currentProcess = new K3Process;
	*kp << m_archiver_program << "v" << m_filename;
	//kp->setUseShell( true );

	kDebug() << "AceArch::open(): kp->args(): " << kp->args() << endl;

	connect( kp, SIGNAL( receivedStdout(K3Process*, char*, int) ),
			 SLOT( slotReceivedTOC(K3Process*, char*, int) ) );
	connect( kp, SIGNAL( receivedStderr(K3Process*, char*, int) ),
			 SLOT( slotReceivedOutput(K3Process*, char*, int) ) );
	connect( kp, SIGNAL( processExited(K3Process*) ),
			 SLOT( slotOpenExited(K3Process*) ) );

	connect( kp, SIGNAL( receivedStdout(K3Process*, char*, int) ),
			 this, SLOT( catchMeIfYouCan(K3Process*, char*, int) ) );

	if ( !kp->start( K3Process::NotifyOnExit, K3Process::AllOutput ) )
	{
		KMessageBox::error( 0, i18n( "Could not start a subprocess." ) );
		emit sigOpen( this, false, QString(), 0 );
	}
}

void AceArch::create()
{
	emit sigCreate( this, true, m_filename,
	                Arch::Extract | Arch::View );
}

void AceArch::addFile( const QStringList & urls )
{
	Q_UNUSED( urls );
}

void AceArch::addDir( const QString & dirName )
{
	Q_UNUSED( dirName );
}

void AceArch::remove( QStringList *list )
{
	Q_UNUSED( list );
}

void AceArch::unarchFileInternal( )
{
	if ( m_destDir.isEmpty() || m_destDir.isNull() )
	{
		kError( 1601 ) << "There was no extract directory given." << endl;
		return;
	}

	K3Process *kp = m_currentProcess = new K3Process;
	kp->clearArguments();

	// extract (and maybe overwrite)
	*kp << m_unarchiver_program << "x" << "-y";

	if ( ArkSettings::extractOverwrite() )
	{
		*kp << "-o";
	}

	*kp << m_filename;

	*kp << m_destDir ;

	// if the file list is empty, no filenames go on the command line,
	// and we then extract everything in the archive.
	if ( m_fileList )
	{
		QStringList::Iterator it;
		for ( it = m_fileList->begin(); it != m_fileList->end(); ++it )
		{
			*kp << (*it);
		}
	}

	connect( kp, SIGNAL( receivedStdout(K3Process*, char*, int) ),
			 SLOT( slotReceivedOutput(K3Process*, char*, int) ) );
	connect( kp, SIGNAL( receivedStderr(K3Process*, char*, int) ),
			 SLOT( slotReceivedOutput(K3Process*, char*, int) ) );
	connect( kp, SIGNAL( processExited(K3Process*) ),
			 SLOT( slotExtractExited(K3Process*) ) );

	if ( !kp->start( K3Process::NotifyOnExit, K3Process::AllOutput ) )
	{
		KMessageBox::error( 0, i18n( "Could not start a subprocess." ) );
		emit sigExtract( false );
	}
}

void AceArch::catchMeIfYouCan( K3Process*, char *buffer, int buflen )
{
	QString myBuf = QString::fromLatin1( buffer, buflen );
	kDebug(1601) << "	Wololo!:	" << myBuf << endl;
}

#include "ace.moc"
// kate: space-indent off; mixedindent off; indent-mode cstyle;
