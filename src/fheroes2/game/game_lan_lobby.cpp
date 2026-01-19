/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2026                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cursor.h"
#include "dialog.h"
#include "localevent.h"
#include "screen.h"
#include "settings.h"
#include "translations.h"
#include "ui_text.h"
#include "ui_tool.h"
#include "ui_window.h"

#include "network/lan_lobby.h"

namespace
{
    enum class LobbyViewMode
    {
        Host,
        Join
    };

    std::string privacyToString( const Network::LobbyPrivacy privacy )
    {
        return ( privacy == Network::LobbyPrivacy::InviteOnly ) ? _( "Invite only" ) : _( "Open" );
    }

    void drainChat( Network::LanLobbyHost & host, std::deque<Network::LobbyChatMessage> & chatLog, bool & changed )
    {
        while ( true ) {
            auto msg = host.popChat();
            if ( !msg ) {
                break;
            }
            chatLog.emplace_back( std::move( *msg ) );
            changed = true;
        }
    }

    void drainChat( Network::LanLobbyClient & client, std::deque<Network::LobbyChatMessage> & chatLog, bool & changed )
    {
        while ( true ) {
            auto msg = client.popChat();
            if ( !msg ) {
                break;
            }
            chatLog.emplace_back( std::move( *msg ) );
            changed = true;
        }
    }

    void trimChat( std::deque<Network::LobbyChatMessage> & chatLog, const size_t maxItems )
    {
        while ( chatLog.size() > maxItems ) {
            chatLog.pop_front();
        }
    }

    void drawChat( const fheroes2::Rect & roi, const std::deque<Network::LobbyChatMessage> & chatLog, fheroes2::Image & output )
    {
        const fheroes2::FontType font( fheroes2::FontSize::SMALL, fheroes2::FontColor::WHITE );
        const int32_t lineHeight = fheroes2::Text( std::string(), font ).height();

        int32_t y = roi.y + 6;

        // Draw from the end.
        const int32_t maxLines = std::max( 1, ( roi.height - 12 ) / std::max( 1, lineHeight ) );
        const int32_t startIndex = std::max<int32_t>( 0, static_cast<int32_t>( chatLog.size() ) - maxLines );

        for ( int32_t i = startIndex; i < static_cast<int32_t>( chatLog.size() ); ++i ) {
            const auto & msg = chatLog[static_cast<size_t>( i )];
            fheroes2::Text line( msg.from + ": " + msg.text, font );
            line.fitToOneRow( roi.width - 12 );
            line.draw( roi.x + 6, y, output );
            y += lineHeight;
            if ( y > roi.y + roi.height - lineHeight ) {
                break;
            }
        }
    }

    void drawSingleLineTextInRoi( const std::string & text, const fheroes2::FontType & font, const fheroes2::Rect & roi, fheroes2::Image & output )
    {
        fheroes2::Text line( text, font );
        line.fitToOneRow( std::max( 1, roi.width - 8 ) );
        line.drawInRoi( roi.x + 4, roi.y + 4, output, roi );
    }

    bool inputText( const std::string & title, const std::string & body, std::string & value, const size_t limit, const bool multiline = false )
    {
        return Dialog::inputString( fheroes2::Text( title, fheroes2::FontType::normalYellow() ), fheroes2::Text( body, fheroes2::FontType::normalWhite() ), value, limit,
                                    multiline, {} );
    }

    bool hostInfoEquals( const Network::LobbyHostInfo & a, const Network::LobbyHostInfo & b )
    {
        return a.lobbyId == b.lobbyId && a.endpoint.address == b.endpoint.address && a.endpoint.port == b.endpoint.port;
    }

    void mergeDiscovered( std::vector<Network::LobbyHostInfo> & dst, std::vector<Network::LobbyHostInfo> && incoming, bool & changed )
    {
        for ( auto & item : incoming ) {
            const auto it = std::find_if( dst.begin(), dst.end(), [&item]( const Network::LobbyHostInfo & existing ) { return hostInfoEquals( existing, item ); } );
            if ( it == dst.end() ) {
                dst.emplace_back( std::move( item ) );
                changed = true;
            }
        }
    }
}

fheroes2::GameMode Game::LocalLanLobby()
{
    fheroes2::Display & display = fheroes2::Display::instance();

    const CursorRestorer cursorRestorer( true, Cursor::POINTER );

    const int32_t windowWidth = std::min<int32_t>( 640, std::max<int32_t>( 520, display.width() - 40 ) );
    const int32_t windowHeight = std::min<int32_t>( 460, std::max<int32_t>( 360, display.height() - 60 ) );

    fheroes2::StandardWindow window( windowWidth, windowHeight, true, display );
    window.applyGemDecoratedCorners();

    const fheroes2::Rect active = window.activeArea();

    const int32_t outerPadding = 12;
    const int32_t topOffset = 34;
    const int32_t bottomOffset = 60;
    const int32_t gap = 10;

    const int32_t panelHeight = std::max( 120, active.height - topOffset - bottomOffset );
    const int32_t leftWidth = std::max( 200, ( active.width - 2 * outerPadding - gap ) / 2 );

    const fheroes2::Rect leftPanel( active.x + outerPadding, active.y + topOffset, leftWidth, panelHeight );
    const fheroes2::Rect chatPanel( leftPanel.x + leftPanel.width + gap, leftPanel.y, active.x + active.width - outerPadding - ( leftPanel.x + leftPanel.width + gap ),
                                    panelHeight );

    const int32_t chatInputOuterHeight = 26;
    const fheroes2::Rect chatInputOuter{ chatPanel.x + 6, chatPanel.y + chatPanel.height - 6 - chatInputOuterHeight, chatPanel.width - 12, chatInputOuterHeight };
    const fheroes2::Rect chatInputArea{ chatInputOuter.x + 4, chatInputOuter.y + 4, chatInputOuter.width - 8, chatInputOuter.height - 8 };

    const fheroes2::FontType hintFont( fheroes2::FontSize::SMALL, fheroes2::FontColor::GRAY );
    const int32_t chatHintHeight = std::max( 1, fheroes2::Text( std::string(), hintFont ).height() );
    const fheroes2::Rect chatHintRoi{ chatPanel.x + 6, chatInputOuter.y - chatHintHeight - 2, chatPanel.width - 12, chatHintHeight };
    const fheroes2::Rect chatLogArea{ chatPanel.x, chatPanel.y, chatPanel.width, std::max( 20, chatHintRoi.y - chatPanel.y - 4 ) };

    window.applyTextBackgroundShading( leftPanel );
    window.applyTextBackgroundShading( chatPanel );

    // Static borders for better visual structure.
    fheroes2::DrawRect( display, chatInputOuter, 51 );

    // Capture panel backgrounds (shaded) for fast redraw.
    fheroes2::ImageRestorer leftRestorer( display, leftPanel.x, leftPanel.y, leftPanel.width, leftPanel.height );
    fheroes2::ImageRestorer chatRestorer( display, chatPanel.x, chatPanel.y, chatPanel.width, chatPanel.height );

    LobbyViewMode viewMode = LobbyViewMode::Join;

    std::string playerName = _( "Player" );
    std::string lobbyName = _( "My Lobby" );
    Network::LobbyPrivacy privacy = Network::LobbyPrivacy::Open;
    std::string inviteCode;

    Network::LanLobbyHost host;
    Network::LanLobbyClient client;
    client.startDiscovery();

    std::vector<Network::LobbyHostInfo> discovered;
    int32_t selectedLobby = -1;
    int32_t discoveredScroll = 0;

    std::optional<Network::LobbyHostInfo> connectedHost;

    // These get recomputed when drawing Join view.
    fheroes2::Rect discoveredListRoi;
    int32_t discoveredRowHeight = 0;
    int32_t discoveredMaxRows = 0;

    std::deque<Network::LobbyChatMessage> chatLog;

    std::string chatInputText;
    size_t chatCursorPos = 0;
    bool chatInputFocused = true;
    fheroes2::TextInputField chatInput( chatInputArea, false, false, display );

    fheroes2::ButtonSprite buttonHost;
    fheroes2::ButtonSprite buttonJoin;
    fheroes2::ButtonSprite buttonBack;
    fheroes2::ButtonSprite buttonSend;

    fheroes2::ButtonSprite buttonAction;
    fheroes2::ButtonSprite buttonSetName;
    fheroes2::ButtonSprite buttonSetLobby;
    fheroes2::ButtonSprite buttonPrivacy;
    fheroes2::ButtonSprite buttonInvite;

    const fheroes2::FontType headerFont = fheroes2::FontType::normalYellow();

    // Header ROI (unshaded area above the chat panel) for dynamic status line.
    const int32_t headerTextHeight = std::max( 1, fheroes2::Text( std::string(), headerFont ).height() );
    const fheroes2::Rect chatHeaderRoi{ chatPanel.x, chatPanel.y - headerTextHeight - 4, chatPanel.width, headerTextHeight + 4 };
    fheroes2::ImageRestorer chatHeaderRestorer( display, chatHeaderRoi.x, chatHeaderRoi.y, chatHeaderRoi.width, chatHeaderRoi.height );

    // Static headers.
    {
        fheroes2::Text title( _( "Local LAN Lobby" ), headerFont );
        title.draw( active.x + ( active.width - title.width() ) / 2, active.y + 8, display );

        fheroes2::Text leftHeader( _( "Lobby" ), headerFont );
        leftHeader.draw( leftPanel.x + 6, leftPanel.y - leftHeader.height() - 2, display );
    }

    auto renderChatHeader = [&]() {
        chatHeaderRestorer.restore();

        std::string line = _( "Chat" );
        line += " - ";
        line += _( "You:" );
        line += " ";
        line += playerName;

        if ( viewMode == LobbyViewMode::Host ) {
            line += " - ";
            line += host.isRunning() ? _( "Hosting" ) : _( "Not hosting" );
        }
        else {
            line += " - ";
            line += client.isConnected() ? _( "Connected" ) : _( "Not connected" );

            if ( connectedHost ) {
                line += " (";
                line += connectedHost->endpoint.address;
                line += ":";
                line += std::to_string( connectedHost->endpoint.port );
                line += ")";
            }
        }

        fheroes2::Text chatHeader( line, headerFont );
        chatHeader.fitToOneRow( chatHeaderRoi.width - 12 );
        chatHeader.drawInRoi( chatHeaderRoi.x + 6, chatHeaderRoi.y + 2, display, chatHeaderRoi );

        display.render( chatHeaderRoi );
    };

    auto renderTabs = [&]() {
        const fheroes2::Point offset{ 18, 8 };
        window.renderTextAdaptedButtonSprite( buttonHost, _( "Host" ), offset, fheroes2::StandardWindow::Padding::TOP_LEFT );

        const fheroes2::Point joinOffset{ ( buttonHost.area().x - active.x ) + buttonHost.area().width + 8, offset.y };
        window.renderTextAdaptedButtonSprite( buttonJoin, _( "Join" ), joinOffset, fheroes2::StandardWindow::Padding::TOP_LEFT );

        buttonHost.drawOnState( viewMode == LobbyViewMode::Host );
        buttonJoin.drawOnState( viewMode == LobbyViewMode::Join );
    };

    auto renderBottomButtons = [&]() {
        window.renderTextAdaptedButtonSprite( buttonSend, _( "Send" ), { 20, 7 }, fheroes2::StandardWindow::Padding::BOTTOM_LEFT );
        window.renderTextAdaptedButtonSprite( buttonBack, _( "Back" ), { 20, 7 }, fheroes2::StandardWindow::Padding::BOTTOM_RIGHT );
    };

    auto renderLeftPanel = [&]() {
        leftRestorer.restore();

        // Mode-specific buttons must be explicitly toggled to avoid drawing stale sprites.
        buttonSetLobby.disable();
        buttonPrivacy.disable();

        const int32_t x = leftPanel.x + 8;
        int32_t y = leftPanel.y + 8;

        if ( viewMode == LobbyViewMode::Host ) {
            const std::string actionText = host.isRunning() ? _( "Stop hosting" ) : _( "Start hosting" );
            window.renderTextAdaptedButtonSprite( buttonAction, actionText.c_str(), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonAction.area().height + 6;

            window.renderTextAdaptedButtonSprite( buttonSetName, _( "Set name" ), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonSetName.area().height + 6;

            window.renderTextAdaptedButtonSprite( buttonSetLobby, _( "Set lobby" ), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            buttonSetLobby.enable();
            y += buttonSetLobby.area().height + 6;

            const std::string privacyLabel = _( "Privacy: " ) + privacyToString( privacy );
            window.renderTextAdaptedButtonSprite( buttonPrivacy, privacyLabel.c_str(), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            buttonPrivacy.enable();
            y += buttonPrivacy.area().height + 6;

            window.renderTextAdaptedButtonSprite( buttonInvite, _( "Set invite code" ), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonInvite.area().height + 10;

            const fheroes2::FontType font( fheroes2::FontSize::SMALL, fheroes2::FontColor::WHITE );
            const fheroes2::FontType highlightFont( fheroes2::FontSize::SMALL, fheroes2::FontColor::YELLOW );

            fheroes2::Text info1( _( "Name:" ) + std::string( " " ) + playerName, font );
            info1.fitToOneRow( leftPanel.width - 16 );
            info1.draw( x, y, display );
            y += info1.height();

            fheroes2::Text info2( _( "Lobby:" ) + std::string( " " ) + lobbyName, font );
            info2.fitToOneRow( leftPanel.width - 16 );
            info2.draw( x, y, display );
            y += info2.height();

            const std::string status = host.isRunning() ? ( _( "Hosting on port " ) + std::to_string( host.tcpPort() ) ) : _( "Not hosting" );
            fheroes2::Text info3( status, highlightFont );
            info3.fitToOneRow( leftPanel.width - 16 );
            info3.draw( x, y, display );
            y += info3.height() + 6;

            if ( privacy == Network::LobbyPrivacy::InviteOnly ) {
                const std::string code = inviteCode.empty() ? _( "(not set)" ) : inviteCode;
                fheroes2::Text invite( _( "Invite code:" ) + std::string( " " ) + code, highlightFont );
                invite.fitToOneRow( leftPanel.width - 16 );
                invite.draw( x, y, display );
            }
            else {
                fheroes2::Text invite( _( "Invite code:" ) + std::string( " - " ) + _( "not required" ), font );
                invite.fitToOneRow( leftPanel.width - 16 );
                invite.draw( x, y, display );
            }
        }
        else {
            const std::string actionText = client.isConnected() ? _( "Disconnect" ) : _( "Connect" );
            window.renderTextAdaptedButtonSprite( buttonAction, actionText.c_str(), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonAction.area().height + 6;

            window.renderTextAdaptedButtonSprite( buttonSetName, _( "Set name" ), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonSetName.area().height + 6;

            window.renderTextAdaptedButtonSprite( buttonInvite, _( "Set invite code" ), { x - active.x, y - active.y }, fheroes2::StandardWindow::Padding::TOP_LEFT );
            y += buttonInvite.area().height + 10;

            const fheroes2::FontType font( fheroes2::FontSize::SMALL, fheroes2::FontColor::WHITE );

            fheroes2::Text info1( _( "Name:" ) + std::string( " " ) + playerName, font );
            info1.fitToOneRow( leftPanel.width - 16 );
            info1.draw( x, y, display );
            y += info1.height() + 6;

            if ( selectedLobby >= 0 && selectedLobby < static_cast<int32_t>( discovered.size() ) ) {
                const auto & sel = discovered[static_cast<size_t>( selectedLobby )];
                const fheroes2::FontType selFont( fheroes2::FontSize::SMALL, fheroes2::FontColor::YELLOW );
                const std::string selLine = _( "Selected:" ) + std::string( " " ) + sel.endpoint.address + ":" + std::to_string( sel.endpoint.port );
                fheroes2::Text selectedInfo( selLine, selFont );
                selectedInfo.fitToOneRow( leftPanel.width - 16 );
                selectedInfo.draw( x, y, display );
                y += selectedInfo.height() + 2;

                if ( sel.privacy == Network::LobbyPrivacy::InviteOnly ) {
                    fheroes2::Text hint( _( "Invite required" ), selFont );
                    hint.fitToOneRow( leftPanel.width - 16 );
                    hint.draw( x, y, display );
                    y += hint.height() + 6;
                }
            }

            fheroes2::Text listHeader( _( "Discovered lobbies:" ), font );
            listHeader.draw( x, y, display );
            y += listHeader.height() + 4;

            discoveredListRoi = { x, y, leftPanel.width - 16, leftPanel.y + leftPanel.height - 8 - y };
            fheroes2::DrawRect( display, discoveredListRoi, 51 );

            const int32_t lineHeight = fheroes2::Text( std::string(), font ).height();
            discoveredRowHeight = std::max( 1, lineHeight + 2 );
            discoveredMaxRows = std::max( 1, discoveredListRoi.height / discoveredRowHeight );

            const int32_t maxScroll = std::max( 0, static_cast<int32_t>( discovered.size() ) - discoveredMaxRows );
            discoveredScroll = std::clamp( discoveredScroll, 0, maxScroll );

            for ( int32_t row = 0; row < discoveredMaxRows; ++row ) {
                const int32_t idx = discoveredScroll + row;
                if ( idx < 0 || idx >= static_cast<int32_t>( discovered.size() ) ) {
                    break;
                }

                const auto & h = discovered[static_cast<size_t>( idx )];

                const fheroes2::Rect rowRoi{ discoveredListRoi.x + 2, discoveredListRoi.y + 2 + row * discoveredRowHeight, discoveredListRoi.width - 4,
                                             discoveredRowHeight };
                if ( idx == selectedLobby ) {
                    fheroes2::DrawRect( display, rowRoi, 51 );
                }

                const std::string line = h.lobbyName + " (" + h.hostPlayerName + ")" + " - " + privacyToString( h.privacy ) + " - " + h.endpoint.address + ":"
                                         + std::to_string( h.endpoint.port );
                fheroes2::Text t( line, font );
                t.fitToOneRow( discoveredListRoi.width - 12 );
                t.drawInRoi( discoveredListRoi.x + 6, rowRoi.y, display, discoveredListRoi );
            }
        }

        display.render( leftPanel );
    };

    auto renderChatPanel = [&]() {
        chatRestorer.restore();

        drawChat( chatLogArea, chatLog, display );

        drawSingleLineTextInRoi( _( "Enter: send   Shift+Enter: newline (later)" ), hintFont, chatHintRoi, display );

        if ( chatInputFocused ) {
            chatInput.draw( chatInputText, static_cast<int32_t>( chatCursorPos ) );
        }
        else {
            const fheroes2::FontType font( fheroes2::FontSize::SMALL, fheroes2::FontColor::WHITE );
            const fheroes2::FontType hintFont( fheroes2::FontSize::SMALL, fheroes2::FontColor::GRAY );
            if ( chatInputText.empty() ) {
                drawSingleLineTextInRoi( _( "Click here to type..." ), hintFont, chatInputOuter, display );
            }
            else {
                drawSingleLineTextInRoi( chatInputText, font, chatInputOuter, display );
            }
        }

        display.render( chatPanel );
    };

    renderTabs();
    renderBottomButtons();
    renderLeftPanel();
    renderChatHeader();
    renderChatPanel();

    fheroes2::validateFadeInAndRender();

    LocalEvent & le = LocalEvent::Get();

    bool needLeftRedraw = false;
    bool needChatRedraw = false;

    while ( le.HandleEvents() ) {
        // Background pumping.
        if ( viewMode == LobbyViewMode::Host ) {
            if ( host.isRunning() ) {
                host.pump();
                drainChat( host, chatLog, needChatRedraw );
            }
        }
        else {
            client.pumpDiscovery();
            mergeDiscovered( discovered, client.drainDiscovered(), needLeftRedraw );

            if ( client.isConnected() ) {
                client.pumpConnection();
                drainChat( client, chatLog, needChatRedraw );
            }
        }

        trimChat( chatLog, 200 );

        // Focus handling for chat input.
        if ( le.MouseClickLeft( chatInputOuter ) ) {
            chatInputFocused = true;
            chatCursorPos = chatInput.getCursorInTextPosition( le.getMouseLeftButtonPressedPos() );
            needChatRedraw = true;
        }
        else if ( le.MouseClickLeft() && !le.isMouseCursorPosInArea( chatInputOuter ) ) {
            if ( chatInputFocused ) {
                chatInputFocused = false;
                needChatRedraw = true;
            }
        }

        buttonHost.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonHost.area() ) );
        buttonJoin.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonJoin.area() ) );
        buttonBack.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonBack.area() ) );
        buttonSend.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonSend.area() ) );

        // Left panel buttons might not exist yet in the current mode (but drawOnState is safe for enabled buttons).
        if ( buttonAction.isEnabled() ) {
            buttonAction.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonAction.area() ) );
        }
        if ( buttonSetName.isEnabled() ) {
            buttonSetName.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonSetName.area() ) );
        }
        if ( buttonSetLobby.isEnabled() ) {
            buttonSetLobby.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonSetLobby.area() ) );
        }
        if ( buttonPrivacy.isEnabled() ) {
            buttonPrivacy.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonPrivacy.area() ) );
        }
        if ( buttonInvite.isEnabled() ) {
            buttonInvite.drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( buttonInvite.area() ) );
        }

        if ( le.MouseClickLeft( buttonBack.area() ) ) {
            if ( host.isRunning() ) {
                host.stop();
            }
            client.disconnect();
            client.stopDiscovery();
            return fheroes2::GameMode::NEW_GAME;
        }

        if ( le.MouseClickLeft( buttonHost.area() ) && viewMode != LobbyViewMode::Host ) {
            viewMode = LobbyViewMode::Host;
            client.disconnect();
            client.stopDiscovery();
            connectedHost.reset();
            discovered.clear();
            selectedLobby = -1;
            needLeftRedraw = true;
            needChatRedraw = true;
            renderTabs();
            renderChatHeader();
            display.render( window.activeArea() );
        }
        if ( le.MouseClickLeft( buttonJoin.area() ) && viewMode != LobbyViewMode::Join ) {
            viewMode = LobbyViewMode::Join;
            if ( host.isRunning() ) {
                host.stop();
            }
            client.startDiscovery();
            needLeftRedraw = true;
            needChatRedraw = true;
            renderTabs();
            renderChatHeader();
            display.render( window.activeArea() );
        }

        if ( viewMode == LobbyViewMode::Join && le.isMouseWheelUpInArea( discoveredListRoi ) ) {
            if ( discoveredScroll > 0 ) {
                --discoveredScroll;
                needLeftRedraw = true;
            }
        }
        if ( viewMode == LobbyViewMode::Join && le.isMouseWheelDownInArea( discoveredListRoi ) ) {
            const int32_t maxScroll = std::max( 0, static_cast<int32_t>( discovered.size() ) - std::max( 1, discoveredMaxRows ) );
            if ( discoveredScroll < maxScroll ) {
                ++discoveredScroll;
                needLeftRedraw = true;
            }
        }

        // Select discovered lobby on click (join mode).
        if ( viewMode == LobbyViewMode::Join && le.MouseClickLeft( discoveredListRoi ) ) {
            const int32_t localY = le.getMouseLeftButtonPressedPos().y - discoveredListRoi.y - 2;
            const int32_t row = ( discoveredRowHeight > 0 ) ? ( localY / discoveredRowHeight ) : 0;
            const int32_t idx = discoveredScroll + row;
            if ( idx >= 0 && idx < static_cast<int32_t>( discovered.size() ) ) {
                selectedLobby = idx;
                needLeftRedraw = true;
            }
        }

        // Left-panel controls.
        if ( le.MouseClickLeft( buttonSetName.area() ) ) {
            std::string tmp = playerName;
            if ( inputText( _( "Player Name" ), _( "Enter your player name:" ), tmp, 32, false ) ) {
                if ( !tmp.empty() ) {
                    playerName = std::move( tmp );
                    needLeftRedraw = true;
                    renderChatHeader();
                }
            }
        }

        if ( viewMode == LobbyViewMode::Host && le.MouseClickLeft( buttonSetLobby.area() ) ) {
            std::string tmp = lobbyName;
            if ( inputText( _( "Lobby Name" ), _( "Enter lobby name:" ), tmp, 32, false ) ) {
                if ( !tmp.empty() ) {
                    lobbyName = std::move( tmp );
                    needLeftRedraw = true;
                }
            }
        }

        if ( viewMode == LobbyViewMode::Host && le.MouseClickLeft( buttonPrivacy.area() ) ) {
            privacy = ( privacy == Network::LobbyPrivacy::Open ) ? Network::LobbyPrivacy::InviteOnly : Network::LobbyPrivacy::Open;
            needLeftRedraw = true;
        }

        if ( le.MouseClickLeft( buttonInvite.area() ) ) {
            std::string tmp = inviteCode;
            if ( inputText( _( "Invite Code" ), _( "Enter invite code (leave empty for none):" ), tmp, 32, false ) ) {
                inviteCode = std::move( tmp );
                needLeftRedraw = true;
            }
        }

        if ( le.MouseClickLeft( buttonAction.area() ) ) {
            if ( viewMode == LobbyViewMode::Host ) {
                if ( host.isRunning() ) {
                    host.stop();
                    needLeftRedraw = true;
                    renderChatHeader();
                }
                else {
                    if ( privacy == Network::LobbyPrivacy::InviteOnly && inviteCode.empty() ) {
                        fheroes2::showStandardTextMessage( _( "Invite only" ), _( "Please set an invite code for an invite-only lobby." ), Dialog::OK );
                    }
                    else if ( !host.start( lobbyName, playerName, privacy, inviteCode ) ) {
                        fheroes2::showStandardTextMessage( _( "Error" ), _( "Failed to start hosting." ), Dialog::OK );
                    }
                    needLeftRedraw = true;
                    renderChatHeader();
                }
            }
            else {
                if ( client.isConnected() ) {
                    client.disconnect();
                    connectedHost.reset();
                    needLeftRedraw = true;
                    needChatRedraw = true;
                    renderChatHeader();
                }
                else {
                    if ( selectedLobby < 0 || selectedLobby >= static_cast<int32_t>( discovered.size() ) ) {
                        fheroes2::showStandardTextMessage( _( "Connect" ), _( "Select a lobby first." ), Dialog::OK );
                    }
                    else {
                        const auto & h = discovered[static_cast<size_t>( selectedLobby )];
                        if ( h.privacy == Network::LobbyPrivacy::InviteOnly && inviteCode.empty() ) {
                            fheroes2::showStandardTextMessage( _( "Invite only" ), _( "This lobby requires an invite code." ), Dialog::OK );
                        }
                        else if ( !client.connectToHost( h, playerName, inviteCode ) ) {
                            fheroes2::showStandardTextMessage( _( "Error" ), _( "Failed to connect." ), Dialog::OK );
                        }
                        else {
                            connectedHost = h;
                        }
                        needLeftRedraw = true;
                        needChatRedraw = true;
                        renderChatHeader();
                    }
                }
            }
        }

        const auto trySendChat = [&]() {
            if ( chatInputText.empty() ) {
                return;
            }

            if ( viewMode == LobbyViewMode::Host && host.isRunning() ) {
                host.sendChatFromHost( chatInputText );
                chatInputText.clear();
                chatCursorPos = 0;
                needChatRedraw = true;
            }
            else if ( viewMode == LobbyViewMode::Join && client.isConnected() ) {
                client.sendChat( chatInputText );
                chatInputText.clear();
                chatCursorPos = 0;
                needChatRedraw = true;
            }
            else {
                fheroes2::showStandardTextMessage( _( "Chat" ), _( "You are not connected to a lobby." ), Dialog::OK );
            }
        };

        if ( le.MouseClickLeft( buttonSend.area() ) ) {
            trySendChat();
        }

        if ( chatInputFocused && le.isAnyKeyPressed() ) {
            const fheroes2::Key key = le.getPressedKeyValue();

            if ( key == fheroes2::Key::KEY_ESCAPE ) {
                chatInputFocused = false;
                needChatRedraw = true;
            }
            if ( key == fheroes2::Key::KEY_ENTER ) {
                trySendChat();
            }
            else if ( chatInputText.size() < 120 || key == fheroes2::Key::KEY_BACKSPACE || key == fheroes2::Key::KEY_DELETE || key == fheroes2::Key::KEY_LEFT
                      || key == fheroes2::Key::KEY_RIGHT || key == fheroes2::Key::KEY_HOME || key == fheroes2::Key::KEY_END ) {
                std::string tmp = chatInputText;
                const size_t newPos = fheroes2::InsertKeySym( tmp, chatCursorPos, key, LocalEvent::getCurrentKeyModifiers() );
                if ( tmp != chatInputText || newPos != chatCursorPos ) {
                    chatInputText = std::move( tmp );
                    chatCursorPos = newPos;
                    needChatRedraw = true;
                }
            }
        }

        if ( needLeftRedraw ) {
            renderLeftPanel();
            needLeftRedraw = false;
        }
        if ( needChatRedraw ) {
            renderChatPanel();
            needChatRedraw = false;
        }
        else if ( chatInputFocused && chatInput.eventProcessing() ) {
            // Cursor blink update.
            display.render( chatInput.getCursorArea() );
        }
    }

    return fheroes2::GameMode::NEW_GAME;
}
