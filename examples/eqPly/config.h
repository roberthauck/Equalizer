
/* Copyright (c) 2006, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#ifndef EQ_PLY_CONFIG_H
#define EQ_PLY_CONFIG_H

#include <eq/eq.h>

#include "frameData.h"
#include "initData.h"

class Config : public eq::Config
{
public:
    Config() : _running( false ) {}
    virtual ~Config(){}

    bool isRunning() const { return _running; }
    
    /** @sa eq::Config::init. */
    virtual bool init( const uint32_t initID );

protected:
    eqNet::Object* instanciateObject( const uint32_t type, const void* data, 
                                       const uint64_t dataSize )
        {
            switch( type )
            {
                case TYPE_INITDATA:
                    return new InitData( data, dataSize );
                case TYPE_FRAMEDATA:
                    return new FrameData( data, dataSize );
                default:
                    return eqNet::Session::instanciateObject( type, data,
                                                              dataSize );
            }
        }

    /** @sa eq::Config::handleEvent */
    virtual bool handleEvent( eq::ConfigEvent* event );

    bool _running;
};

#endif // EQ_PLY_CONFIG_H
