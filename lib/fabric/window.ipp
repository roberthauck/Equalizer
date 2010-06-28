
/* Copyright (c) 2010, Stefan Eilemann <eile@equalizergraphics.com>
 * Copyright (c) 2010, Cedric Stalder <cedric.stalder@gmail.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "window.h"

#include "channel.h"
#include "elementVisitor.h"
#include "packets.h"
#include "task.h"

#include <eq/net/command.h>
#include <eq/net/dataIStream.h>
#include <eq/net/dataOStream.h>

namespace eq
{
namespace fabric
{

namespace
{
#define MAKE_WINDOW_ATTR_STRING( attr ) ( std::string("EQ_WINDOW_") + #attr )
std::string _iAttributeStrings[] = {
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_STEREO ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_DOUBLEBUFFER ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_FULLSCREEN ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_DECORATION ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_SWAPSYNC ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_DRAWABLE ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_STATISTICS ),
    MAKE_WINDOW_ATTR_STRING( IATTR_HINT_SCREENSAVER ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_COLOR ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_ALPHA ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_DEPTH ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_STENCIL ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_ACCUM ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_ACCUM_ALPHA ),
    MAKE_WINDOW_ATTR_STRING( IATTR_PLANES_SAMPLES ),
    MAKE_WINDOW_ATTR_STRING( IATTR_FILL1 ),
    MAKE_WINDOW_ATTR_STRING( IATTR_FILL2 )
};
}

template< class P, class W, class C >
Window< P, W, C >::Window( P* parent )
        : _pipe( parent )
{
    EQASSERT( parent );
    parent->_addWindow( static_cast< W* >( this ) );
    notifyViewportChanged();
}

template< class P, class W, class C >
Window< P, W, C >::~Window( )
{    
    while( !_channels.empty( ))
    {
        C* channel = _channels.back();

        EQASSERT( channel->getWindow() == this );
        _removeChannel( channel );
        delete channel;
    }
    _pipe->_removeWindow( static_cast< W* >( this ) );
}

template< class P, class W, class C >
void Window< P, W, C >::attachToSession( const uint32_t id,
                                         const uint32_t instanceID,
                                         net::Session* session )
{
    Object::attachToSession( id, instanceID, session );

    net::CommandQueue* queue = _pipe->getMainThreadQueue();
    EQASSERT( queue );

    registerCommand( fabric::CMD_WINDOW_NEW_CHANNEL, 
                     CmdFunc( this, &Window< P, W, C >::_cmdNewChannel ),
                     queue );
    registerCommand( fabric::CMD_WINDOW_NEW_CHANNEL_REPLY, 
                     CmdFunc( this, &Window< P, W, C >::_cmdNewChannelReply ),
                     0 );
}

template< class P, class W, class C >
void Window< P, W, C >::backup()
{
    Object::backup();
    _backup = _data;
}

template< class P, class W, class C >
void Window< P, W, C >::restore()
{
    _data = _backup;
    _data.drawableConfig = DrawableConfig();

    Object::restore();
    notifyViewportChanged();
    setDirty( DIRTY_VIEWPORT );
}

template< class P, class W, class C >
void Window< P, W, C >::serialize( net::DataOStream& os,
                                   const uint64_t dirtyBits )
{
    Object::serialize( os, dirtyBits );
    if( dirtyBits & DIRTY_ATTRIBUTES )
        os.write( _data.iAttributes, IATTR_ALL * sizeof( int32_t ));
    if( dirtyBits & DIRTY_CHANNELS )
    {
        os << _mapNodeObjects();
        os.serializeChildren( this, _channels );
    }
    if( dirtyBits & DIRTY_VIEWPORT )
        os << _data.vp << _data.pvp << _data.fixedVP;
    if( dirtyBits & DIRTY_DRAWABLECONFIG )
        os << _data.drawableConfig;
}

template< class P, class W, class C >
void Window< P, W, C >::deserialize( net::DataIStream& is,
                                     const uint64_t dirtyBits )
{
    Object::deserialize( is, dirtyBits );
    if( dirtyBits & DIRTY_ATTRIBUTES )
        is.read( _data.iAttributes, IATTR_ALL * sizeof( int32_t ));
    if( dirtyBits & DIRTY_CHANNELS )
    {
        bool useChildren;
        is >> useChildren;
        if( useChildren && _mapNodeObjects( ))
        {
            Channels result;
            is.deserializeChildren( this, _channels, result );
            if( !isMaster( ))
            {
                _channels.swap( result );
                EQASSERT( _channels.size() == result.size( ));
            }
        }
        else // consume unused ObjectVersions
        {
            net::ObjectVersions childIDs;
            is >> childIDs;
        }
    }
    if( dirtyBits & DIRTY_VIEWPORT )
    {
        // Ignore data from master (server) if we have local changes
        if( !Serializable::isDirty( DIRTY_VIEWPORT ) || isMaster( ))
        {
            is >> _data.vp >> _data.pvp >> _data.fixedVP;
            notifyViewportChanged();
        }
        else // consume unused data
            is.advanceBuffer( sizeof( _data.vp ) + sizeof( _data.pvp ) + sizeof( _data.fixedVP ));
    }

    if( dirtyBits & DIRTY_DRAWABLECONFIG )
        is >> _data.drawableConfig;
}

template< class P, class W, class C >
void Window< P, W, C >::setDirty( const uint64_t dirtyBits )
{
    Object::setDirty( dirtyBits );
    if( isMaster( ))
        _pipe->setDirty( P::DIRTY_WINDOWS );
}

template< class P, class W, class C >
void Window< P, W, C >::notifyDetach()
{
    while( !_channels.empty( ))
    {
        C* channel = _channels.back();
        if( channel->getID() > EQ_ID_MAX )
        {
            EQASSERT( isMaster( ));
            return;
        }

        net::Session* session = getSession();
        EQASSERT( session );

        session->releaseObject( channel );
        if( !isMaster( ))
        {
            _removeChannel( channel );
            _pipe->getServer()->getNodeFactory()->releaseChannel( channel );
        }
    }
}

template< class P, class W, class C >
void Window< P, W, C >::create( C** channel )
{
    *channel = _pipe->getServer()->getNodeFactory()->createChannel( 
        static_cast< W* >( this ));
}

template< class P, class W, class C >
void Window< P, W, C >::release( C* channel )
{
    _pipe->getServer()->getNodeFactory()->releaseChannel( channel );
}

template< class P, class W, class C >
void Window< P, W, C >::_addChannel( C* channel )
{
    EQASSERT( channel->getWindow() == this );
    _channels.push_back( channel );
    setDirty( DIRTY_CHANNELS );
}

template< class P, class W, class C >
bool Window< P, W, C >::_removeChannel( C* channel )
{
    typename Channels::iterator i = stde::find( _channels, channel );
    if( i == _channels.end( ))
        return false;

    _channels.erase( i );
    setDirty( DIRTY_CHANNELS );
    return true;
}

template< class P, class W, class C >
C* Window< P, W, C >::_findChannel( const uint32_t id )
{
    for( typename Channels::const_iterator i = _channels.begin(); 
         i != _channels.end(); ++i )
    {
        C* channel = *i;
        if( channel->getID() == id )
            return channel;
    }
    return 0;
}

template< class P, class W, class C > const std::string&
Window< P, W, C >::getIAttributeString( const IAttribute attr )
{
    return _iAttributeStrings[attr];
}

template< class P, class W, class C >
WindowPath Window< P, W, C >::getPath() const
{
    const P* pipe = getPipe();
    EQASSERT( pipe );
    WindowPath path( pipe->getPath( ));
    
    const typename std::vector< W* >& windows = pipe->getWindows();
    typename std::vector< W* >::const_iterator i = std::find( windows.begin(),
                                                              windows.end(),
                                                              this );
    EQASSERT( i != windows.end( ));
    path.windowIndex = std::distance( windows.begin(), i );
    return path;
}

namespace
{
template< class W, class V >
VisitorResult _accept( W* window, V& visitor )
{ 
    VisitorResult result = visitor.visitPre( window );
    if( result != TRAVERSE_CONTINUE )
        return result;

    const typename W::Channels& channels = window->getChannels();
    for( typename W::Channels::const_iterator i = channels.begin(); 
         i != channels.end(); ++i )
    {
        switch( (*i)->accept( visitor ))
        {
            case TRAVERSE_TERMINATE:
                return TRAVERSE_TERMINATE;

            case TRAVERSE_PRUNE:
                result = TRAVERSE_PRUNE;
                break;
                
            case TRAVERSE_CONTINUE:
            default:
                break;
        }
    }

    switch( visitor.visitPost( window ))
    {
        case TRAVERSE_TERMINATE:
            return TRAVERSE_TERMINATE;

        case TRAVERSE_PRUNE:
            return TRAVERSE_PRUNE;
                
        case TRAVERSE_CONTINUE:
        default:
            break;
    }

    return result;
}
}

template< class P, class W, class C >
VisitorResult Window< P, W, C >::accept( Visitor& visitor )
{
    return _accept( static_cast< W* >( this ), visitor );
}

template< class P, class W, class C >
VisitorResult Window< P, W, C >::accept( Visitor& visitor  ) const
{
    return _accept( static_cast< const W* >( this ), visitor );
}

template< class P, class W, class C >
void Window< P, W, C >::setPixelViewport( const PixelViewport& pvp )
{
    EQASSERT( pvp.isValid( ));
    if( !pvp.isValid( ))
        return;

    _data.fixedVP = false;

    if( pvp == _data.pvp && _data.vp.hasArea( ))
        return;

    _data.pvp = pvp;
    _data.vp.invalidate();

    notifyViewportChanged();
    setDirty( DIRTY_VIEWPORT );
}

template< class P, class W, class C >
void Window< P, W, C >::setViewport( const Viewport& vp )
{
    if( !vp.hasArea())
        return;

    _data.fixedVP = true;

    if( vp == _data.vp && _data.pvp.hasArea( ))
        return;
    _data.vp = vp;
    _data.pvp.invalidate();

    setDirty( DIRTY_VIEWPORT );
    notifyViewportChanged();
}

template< class P, class W, class C >
void Window< P, W, C >::notifyViewportChanged()
{
    const PixelViewport pipePVP = _pipe->getPixelViewport();

    if( _data.fixedVP ) // update pixel viewport
    {
        const PixelViewport oldPVP = _data.pvp;
        _data.pvp = pipePVP;
        _data.pvp.apply( _data.vp );
        if( oldPVP != _data.pvp )
            setDirty( DIRTY_VIEWPORT );
    }
    else           // update viewport
    {
        const Viewport oldVP = _data.vp;
        _data.vp = _data.pvp.getSubVP( pipePVP );
        if( oldVP != _data.vp )
            setDirty( DIRTY_VIEWPORT );
    }

    for( typename Channels::const_iterator i = _channels.begin(); 
         i != _channels.end(); ++i )
    {
        (*i)->notifyViewportChanged();
    }
    EQVERB << getName() << " viewport update: " << _data.vp << ":" << _data.pvp
           << std::endl;
}

template< class P, class W, class C >
void Window< P, W, C >::_setDrawableConfig(const DrawableConfig& drawableConfig)
{ 
    _data.drawableConfig = drawableConfig;
    setDirty( DIRTY_DRAWABLECONFIG );
}

//----------------------------------------------------------------------
// Command handlers
//----------------------------------------------------------------------
template< class P, class W, class C >
net::CommandResult Window< P, W, C >::_cmdNewChannel( net::Command& command )
{
    const WindowNewChannelPacket* packet =
        command.getPacket< WindowNewChannelPacket >();
    
    C* channel = 0;
    create( &channel );
    EQASSERT( channel );

    _pipe->getConfig()->registerObject( channel );
    EQASSERT( channel->getID() <= EQ_ID_MAX );

    WindowNewChannelReplyPacket reply( packet );
    reply.channelID = channel->getID();
    send( command.getNode(), reply ); 
    EQASSERT( reply.channelID <= EQ_ID_MAX );

    return net::COMMAND_HANDLED;
}

template< class P, class W, class C > net::CommandResult
Window< P, W, C >::_cmdNewChannelReply( net::Command& command )
{
    const WindowNewChannelReplyPacket* packet =
        command.getPacket< WindowNewChannelReplyPacket >();
    getLocalNode()->serveRequest( packet->requestID, packet->channelID );

    return net::COMMAND_HANDLED;
}

}
}