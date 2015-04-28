import QtQuick 2.4

Item {
    SystemPalette { id: sysPalette; colorGroup: SystemPalette.Active }
    readonly property alias colors: colors
    readonly property alias layout: layout
    readonly property alias fonts: fonts
    readonly property alias styles: styles
    readonly property alias effects: effects

    Item {
        id: colors
        readonly property color hifiBlue: "#0e7077"
        readonly property color window: sysPalette.window
        readonly property color dialogBackground: sysPalette.window
        //readonly property color dialogBackground: "#00000000"
        readonly property color inputBackground: "white"
        readonly property color background: sysPalette.dark
        readonly property color text: sysPalette.text
        readonly property color disabledText: "gray"
        readonly property color hintText: sysPalette.dark
        readonly property color light: sysPalette.light
        readonly property alias activeWindow: activeWindow
        readonly property alias inactiveWindow: inactiveWindow
        QtObject {
            id: activeWindow
            readonly property color headerBackground: "white"
            readonly property color headerText: "black"
        }
        QtObject {
            id: inactiveWindow
            readonly property color headerBackground: "gray"
            readonly property color headerText: "black"
        }
     }

    QtObject {
        id: fonts
        readonly property real headerPointSize: 24
        readonly property string fontFamily: "Helvetica"
        readonly property real fontSize: 18
    }

    QtObject {
        id: layout
        property int spacing: 8
        property int rowHeight: 40
        property int windowTitleHeight: 48
    }

    QtObject {
        id: styles
        readonly property int borderWidth: 0
        readonly property int borderRadius: borderWidth * 2
    }

    QtObject {
        id: effects
        readonly property int fadeInDuration: 400
    }
}
