/*****************************************************************************
 * ClipWorkflow.cpp : Clip workflow. Will extract a single frame from a VLCMedia
 *****************************************************************************
 * Copyright (C) 2008-2009 the VLMC team
 *
 * Authors: Hugo Beauzee-Luyssen <hugo@vlmc.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <QtDebug>

#include "ClipWorkflow.h"

ClipWorkflow::ClipWorkflow( Clip::Clip* clip, QMutex* condMutex, QWaitCondition* waitCond ) :
                m_clip( clip ),
                m_renderComplete( false ),
                m_buffer( NULL ),
                m_condMutex( condMutex ),
                m_waitCond( waitCond ),
                m_mediaPlayer(NULL),
                m_isReady( false ),
                m_endReached( false )
{
    m_mutex = new QReadWriteLock();
    m_buffer = new unsigned char[VIDEOHEIGHT * VIDEOWIDTH * 4];
    m_initMutex = new QReadWriteLock();
    m_endReachedLock = new QReadWriteLock();
}

ClipWorkflow::~ClipWorkflow()
{
    delete[] m_buffer;
    delete m_mutex;
    delete m_initMutex;
    delete m_endReachedLock;
}

bool    ClipWorkflow::renderComplete() const
{
    QReadLocker     lock( m_mutex );
    return m_renderComplete;
}

unsigned char*    ClipWorkflow::getOutput()
{
    return m_buffer;
}

void    ClipWorkflow::lock( ClipWorkflow* clipWorkflow, void** pp_ret )
{
    //It doesn't seems necessary to lock anything here, since the scheduler
    //will wait until the frame is ready to use it, and doesn't use it after
    //it has asked for a new one.

    //In any case, we give vlc a buffer to render in...
    //If we don't, segmentation fault will catch us and eat our brain !! ahem...
//    qDebug() << "Locking in ClipWorkflow::lock";
    *pp_ret = clipWorkflow->m_buffer;
}

void    ClipWorkflow::unlock( ClipWorkflow* clipWorkflow )
{
    if ( clipWorkflow->m_isReady == true )
    {
        QMutexLocker    lock( clipWorkflow->m_condMutex );

        {
            QWriteLocker    lock2( clipWorkflow->m_mutex );
            clipWorkflow->m_renderComplete = true;
        }

        if ( clipWorkflow->m_mediaPlayer->getPosition() > clipWorkflow->m_clip->getEnd() )
        {
            QWriteLocker    lock2( clipWorkflow->m_endReachedLock );
            clipWorkflow->m_endReached = true;
        }
        clipWorkflow->m_waitCond->wait( clipWorkflow->m_condMutex );
    }
}

void    ClipWorkflow::setRenderComplete()
{

}

void    ClipWorkflow::setVmem()
{
    char        buffer[32];

    //TODO: it would be good if we somehow backup the old media parameters to restore it later.
    m_clip->getParent()->getVLCMedia()->addOption( ":vout=vmem" );
    m_clip->getParent()->getVLCMedia()->setDataCtx( this );
    m_clip->getParent()->getVLCMedia()->setLockCallback( reinterpret_cast<LibVLCpp::Media::lockCallback>( &ClipWorkflow::lock ) );
    m_clip->getParent()->getVLCMedia()->setUnlockCallback( reinterpret_cast<LibVLCpp::Media::unlockCallback>( &ClipWorkflow::unlock ) );
    m_clip->getParent()->getVLCMedia()->addOption( ":vmem-chroma=RV24" );

    sprintf( buffer, ":vmem-width=%i", VIDEOWIDTH );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );

    sprintf( buffer, ":vmem-height=%i", VIDEOHEIGHT );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );

    sprintf( buffer, "vmem-pitch=%i", VIDEOWIDTH * 3 );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );
}

void    ClipWorkflow::initialize( LibVLCpp::MediaPlayer* mediaPlayer )
{
    setVmem();
    m_mediaPlayer = mediaPlayer;
    m_mediaPlayer->setMedia( m_clip->getParent()->getVLCMedia() );

    connect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( setPositionAfterPlayback() ), Qt::DirectConnection );
    connect( m_mediaPlayer, SIGNAL( endReached() ), this, SLOT( endReached() ), Qt::DirectConnection );
//    qDebug() << "Launching playback";
    m_mediaPlayer->play();
}

void    ClipWorkflow::setPositionAfterPlayback()
{
//    qDebug() << "Setting position";
    disconnect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( setPositionAfterPlayback() ) );
    connect( m_mediaPlayer, SIGNAL( positionChanged() ), this, SLOT( pauseAfterPlaybackStarted() ), Qt::DirectConnection );
    m_mediaPlayer->setPosition( m_clip->getBegin() );
}

void    ClipWorkflow::pauseAfterPlaybackStarted()
{
    disconnect( m_mediaPlayer, SIGNAL( positionChanged() ), this, SLOT( pauseAfterPlaybackStarted() ) );
    disconnect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( pauseAfterPlaybackStarted() ) );

    connect( m_mediaPlayer, SIGNAL( paused() ), this, SLOT( pausedMediaPlayer() ), Qt::DirectConnection );
//    qDebug() << "pausing media";
    m_mediaPlayer->pause();

}

void    ClipWorkflow::pausedMediaPlayer()
{
    disconnect( m_mediaPlayer, SIGNAL( paused() ), this, SLOT( pausedMediaPlayer() ) );
    QWriteLocker        lock( m_initMutex);
    m_isReady = true;
}

bool    ClipWorkflow::isReady() const
{
    QReadLocker lock( m_initMutex );
    return m_isReady;
}

bool    ClipWorkflow::isEndReached() const
{
    QReadLocker lock( m_endReachedLock );
    return m_endReached;
}

void    ClipWorkflow::startRender()
{
    bool        isReady;
    {
        QReadLocker lock( m_initMutex );
        isReady = m_isReady;
    }
    while ( isReady == false )
    {
        usleep( 150 );
        {
            QReadLocker lock( m_initMutex );
            isReady = m_isReady;
        }
    }
    m_mediaPlayer->play();
}

void    ClipWorkflow::endReached()
{
    QWriteLocker    lock2( m_endReachedLock );
    m_endReached = true;
}

const Clip*     ClipWorkflow::getClip() const
{
    return m_clip;
}

void            ClipWorkflow::stop()
{
    {
        QWriteLocker    lock2( m_mutex );
        m_renderComplete = false;
    }
    {
        QWriteLocker    lock2( m_endReachedLock );
        m_endReached = false;
    }
    {
        QWriteLocker        lock( m_initMutex);
        m_isReady = false;
    }
    m_mediaPlayer->stop();
    m_mediaPlayer = NULL;
//    qDebug() << "Stoped ClipWorkflow";
}

void            ClipWorkflow::setPosition( float pos )
{
    m_mediaPlayer->setPosition( pos );
}
