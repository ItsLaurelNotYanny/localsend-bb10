import bb.cascades 1.2

Container {
    id: cardRoot
    property string peerFingerprint: ""
    property string peerAlias: ""
    property string peerModel: ""
    property string peerAddress: ""
    property bool isHttps: false

    topPadding: 16.0
    bottomPadding: 16.0
    leftPadding: 16.0
    rightPadding: 16.0
    bottomMargin: 12.0
    background: Color.create("#F7FAFC")

    Container {
        layout: StackLayout {
            orientation: LayoutOrientation.LeftToRight
        }

        // Left: Device details
        Container {
            layoutProperties: StackLayoutProperties {
                spaceQuota: 3.0
            }

            Label {
                text: cardRoot.peerAlias
                textStyle.base: SystemDefaults.TextStyles.TitleText
                textStyle.color: Color.create("#2D3748")
            }
            Label {
                text: cardRoot.peerModel + " • " + cardRoot.peerAddress
                textStyle.base: SystemDefaults.TextStyles.SmallText
                textStyle.color: Color.create("#718096")
            }
            Label {
                visible: cardRoot.isHttps
                text: "HTTPS (TLS 1.3)"
                textStyle.base: SystemDefaults.TextStyles.SmallText
                textStyle.color: Color.create("#38A169")
            }
        }

        // Right: Send button
        Button {
            text: "Send"
            enabled: app.hasSelectedFile && !app.transferActive
            layoutProperties: StackLayoutProperties {
                spaceQuota: 1.0
            }
            verticalAlignment: VerticalAlignment.Center
            onClicked: {
                app.sendSelectedFile(cardRoot.peerFingerprint);
            }
        }
    }
}
