/**
 * @file driver.cpp
 */

#include "driver.hpp"

#include "layouts.hpp"

#include "../model/cell-grid.hpp"
#include "../modreg/keymap.hpp"

#include <sstream>

namespace lens {

Driver::Driver( const std::string& layout, int width, int height )
{
    m_model.setGeometry( width, height );
    registerShellCommands( m_model );
    m_model.keymap() = defaultKeymap();
    applyStockLayout( m_model, layout );
    m_model.rebuildHelp();
    record( "<start>" );
}


bool Driver::press( const std::string& keys )
{
    KeySeq seq;
    if( !parseKeySeq( keys, seq ) ) {
        return false;
    }

    for( std::size_t i = 0; i < seq.size(); ++i ) {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = seq[i];
        ( void ) fold( m_model, event );
        record( keys );
    }
    return true;
}


void Driver::pressChar( char32_t ch )
{
    Event event;
    event.kind = Event::Kind::Key;
    event.key = Key::character( ch );
    ( void ) fold( m_model, event );
    record( encodeUtf8( ch ) );
}


std::string Driver::trace( std::size_t tail ) const
{
    std::ostringstream os;
    const std::size_t from = m_steps.size() > tail ? m_steps.size() - tail : 0;

    for( std::size_t i = from; i < m_steps.size(); ++i ) {
        const Step& step = m_steps[i];
        os << "\n          " << i << "  " << step.key
           << "  panel=" << step.focusedPanel
           << " tiles=" << step.tiles;
        if( step.scrollValid ) {
            os << " line=" << step.scroll.cursorLine
               << " top=" << step.scroll.topLine
               << " row=" << step.scroll.cursorScreenRow()
               << " rows=" << step.scroll.viewportRows
               << " of=" << step.scroll.totalLines;
        }
    }
    return os.str();
}


void Driver::record( const std::string& key )
{
    Step step;
    step.key = key;
    step.scroll = m_model.observeFocusedScroll( step.scrollValid );
    step.tiles = m_model.layout().tileCount();
    step.message = m_model.message();

    const Buffer* focused = m_model.focusedBuffer();
    if( focused ) {
        switch( focused->kind ) {
        case PanelKind::Help:        step.focusedPanel = "Help"; break;
        case PanelKind::Menu:        step.focusedPanel = "Menu"; break;
        case PanelKind::Palette:     step.focusedPanel = "Palette"; break;
        case PanelKind::Transcript:  step.focusedPanel = "Transcript"; break;
        case PanelKind::Placeholder: step.focusedPanel = focused->title; break;
        }
    }

    m_steps.push_back( step );
}

} // namespace lens
