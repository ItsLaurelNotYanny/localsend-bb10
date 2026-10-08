import bb.cascades 1.2

TabbedPane {
    showTabsOnActionBar: true

    // TAB 1: SEND & RADAR
    Tab {
        title: "Send"
        Page {
            titleBar: TitleBar {
                title: "LocalSend BB10"
            }
            actions: [
                ActionItem {
                    title: "Refresh"
                    ActionBar.placement: ActionBarPlacement.OnBar
                    onTriggered: {
                        app.refreshDevices();
                    }
                },
                ActionItem {
                    title: "Clear File"
                    enabled: app.hasSelectedFile
                    onTriggered: {
                        app.clearSelectedFile();
                    }
                }
            ]

            ScrollView {
                Container {
                    topPadding: 24.0
                    bottomPadding: 24.0
                    leftPadding: 24.0
                    rightPadding: 24.0

                    // Local device identity banner
                    Container {
                        topPadding: 16.0
                        bottomPadding: 16.0
                        leftPadding: 16.0
                        rightPadding: 16.0
                        background: Color.create("#2D3748")

                        Label {
                            text: app.deviceAlias
                            textStyle.base: SystemDefaults.TextStyles.TitleText
                            textStyle.color: Color.White
                        }
                        Label {
                            text: "Port " + app.devicePort + " • LocalSend Protocol v2.2"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#A0AEC0")
                        }
                    }

                    Divider {}

                    // Selected file section
                    Header {
                        title: "File to Send"
                    }

                    Container {
                        topPadding: 12.0
                        bottomPadding: 12.0

                        Container {
                            visible: app.hasSelectedFile
                            topPadding: 12.0
                            bottomPadding: 12.0
                            leftPadding: 16.0
                            rightPadding: 16.0
                            background: Color.create("#EDF2F7")

                            Label {
                                text: app.selectedFileName
                                textStyle.base: SystemDefaults.TextStyles.BodyText
                                textStyle.color: Color.create("#1A202C")
                            }
                            Label {
                                text: "Size: " + app.selectedFileSize
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.create("#718096")
                            }
                        }

                        Button {
                            text: app.hasSelectedFile ? "Change Selected File" : "Choose File to Send"
                            horizontalAlignment: HorizontalAlignment.Fill
                            onTriggered: {
                                app.chooseFile();
                            }
                        }
                    }

                    Divider {}

                    // Nearby devices section
                    Header {
                        title: "Nearby Devices (" + app.peers.length + ")"
                    }

                    Label {
                        visible: app.peers.length === 0
                        text: "Scanning for LocalSend peers on Wi-Fi..."
                        textStyle.base: SystemDefaults.TextStyles.SubtitleText
                        textStyle.color: Color.create("#718096")
                    }

                    // Peer list rendering
                    Container {
                        id: peersContainer
                        topPadding: 8.0
                        bottomPadding: 8.0

                        // Dynamic peer items built from app.peers
                        // Each item has device alias, model, address, and a Send button
                    }

                    Divider {}

                    // Manual IP connection
                    Header {
                        title: "Manual IP Connection"
                    }

                    Container {
                        layout: StackLayout {
                            orientation: LayoutOrientation.LeftToRight
                        }
                        topPadding: 8.0

                        TextField {
                            id: manualIpField
                            hintText: "IP address (e.g. 192.168.1.50)"
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 3.0
                            }
                        }
                        TextField {
                            id: manualPortField
                            text: "53317"
                            hintText: "Port"
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 1.0
                            }
                        }
                    }

                    Button {
                        text: "Connect to Device"
                        horizontalAlignment: HorizontalAlignment.Fill
                        onTriggered: {
                            app.addManualPeer(manualIpField.text, parseInt(manualPortField.text));
                            manualIpField.text = "";
                        }
                    }

                    // Error banner
                    Label {
                        visible: app.lastError.length > 0
                        text: app.lastError
                        textStyle.color: Color.create("#E53E3E")
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        topMargin: 12.0
                    }
                }
            }
        }
    }

    // TAB 2: TRANSFERS & RECEIVE
    Tab {
        title: "Transfers"
        Page {
            titleBar: TitleBar {
                title: "Transfer Activity"
            }

            ScrollView {
                Container {
                    topPadding: 24.0
                    bottomPadding: 24.0
                    leftPadding: 24.0
                    rightPadding: 24.0

                    // Incoming transfer request prompt
                    Container {
                        visible: app.hasIncomingOffer
                        topPadding: 16.0
                        bottomPadding: 16.0
                        leftPadding: 16.0
                        rightPadding: 16.0
                        background: Color.create("#FEFCBF")

                        Label {
                            text: "Incoming File Transfer!"
                            textStyle.base: SystemDefaults.TextStyles.TitleText
                            textStyle.color: Color.create("#B7791F")
                        }
                        Label {
                            text: "From: " + app.incomingPeerAlias
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.color: Color.create("#2D3748")
                        }
                        Label {
                            text: "File: " + app.incomingFileName + " (" + app.incomingFileSize + ")"
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.color: Color.create("#2D3748")
                        }

                        Container {
                            layout: StackLayout {
                                orientation: LayoutOrientation.LeftToRight
                            }
                            topPadding: 12.0

                            Button {
                                text: "Accept"
                                color: Color.create("#38A169")
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onTriggered: {
                                    app.acceptIncoming(app.incomingRequestId, app.incomingFileId);
                                }
                            }
                            Button {
                                text: "Decline"
                                color: Color.create("#E53E3E")
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onTriggered: {
                                    app.rejectIncoming(app.incomingRequestId);
                                }
                            }
                        }
                    }

                    Divider {
                        visible: app.hasIncomingOffer
                    }

                    // Active transfer status
                    Header {
                        title: "Current Transfer Status"
                    }

                    Container {
                        topPadding: 12.0
                        bottomPadding: 12.0
                        leftPadding: 16.0
                        rightPadding: 16.0
                        background: Color.create("#EDF2F7")

                        Label {
                            text: "State: " + app.transferState
                            textStyle.base: SystemDefaults.TextStyles.TitleText
                            textStyle.color: Color.create("#2B6CB0")
                        }

                        Label {
                            text: app.transferStatusText
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.color: Color.create("#4A5568")
                        }

                        // Progress indicator
                        ProgressBar {
                            value: app.transferProgress
                            toValue: 100.0
                            topMargin: 12.0
                            bottomMargin: 12.0
                        }

                        Button {
                            text: "Cancel Transfer"
                            enabled: app.transferActive
                            color: Color.create("#E53E3E")
                            horizontalAlignment: HorizontalAlignment.Fill
                            onTriggered: {
                                app.cancelTransfer();
                            }
                        }
                    }

                    Divider {}

                    Header {
                        title: "Receive Directory"
                    }
                    Label {
                        text: app.receiveDirectory
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#718096")
                    }
                }
            }
        }
    }

    // TAB 3: SETTINGS & ABOUT
    Tab {
        title: "Settings"
        Page {
            titleBar: TitleBar {
                title: "Settings"
            }

            ScrollView {
                Container {
                    topPadding: 24.0
                    bottomPadding: 24.0
                    leftPadding: 24.0
                    rightPadding: 24.0

                    Header {
                        title: "Device Name"
                    }

                    TextField {
                        id: aliasEditField
                        text: app.deviceAlias
                        hintText: "Enter device name"
                    }

                    Button {
                        text: "Save Device Name"
                        horizontalAlignment: HorizontalAlignment.Fill
                        onTriggered: {
                            app.setDeviceAlias(aliasEditField.text);
                        }
                    }

                    Divider {}

                    Header {
                        title: "Storage Location"
                    }
                    Label {
                        text: "Received files are saved to:"
                        textStyle.base: SystemDefaults.TextStyles.BodyText
                    }
                    Label {
                        text: app.receiveDirectory
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#4A5568")
                    }

                    Divider {}

                    Header {
                        title: "About LocalSend for BB10"
                    }
                    Label {
                        text: "LocalSend BlackBerry 10 Edition"
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                    }
                    Label {
                        text: "Version 0.1.0 (MVP)\nProtocol v2.2 Compliant\nNative Qt 4.8.5 & Cascades C++03"
                        textStyle.base: SystemDefaults.TextStyles.BodyText
                        textStyle.color: Color.create("#718096")
                    }
                    Label {
                        text: "Share files seamlessly across devices on your local Wi-Fi network without Internet access."
                        textStyle.base: SystemDefaults.TextStyles.SubtitleText
                        textStyle.color: Color.create("#A0AEC0")
                        topMargin: 8.0
                    }
                }
            }
        }
    }
}
