import bb.cascades 1.2

TabbedPane {
    showTabsOnActionBar: true

    // TAB 1: SEND & RADAR
    Tab {
        title: "Send"
        Page {
            titleBar: TitleBar {
                title: "LocalSend BB10"
                acceptAction: ActionItem {
                    title: app.isScanning ? "Scanning..." : "Scan"
                    enabled: !app.isScanning
                    onTriggered: {
                        app.refreshDevices();
                    }
                }
            }
            actions: [
                ActionItem {
                    title: app.isScanning ? "Scanning..." : "Scan"
                    enabled: !app.isScanning
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
                            text: (app.isHttps ? "HTTPS • " : "HTTP • ") + app.networkStatus + " • Port " + app.devicePort + " • " + app.appVersion
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#68D391")
                        }
                        Label {
                            visible: app.certificateFingerprint.length > 0
                            text: "SHA-256: " + app.certificateFingerprint
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#CBD5E0")
                        }
                    }

                    Divider {}

                    // Incoming transfer alert banner on Radar tab
                    Container {
                        visible: app.hasIncomingOffer
                        topPadding: 16.0
                        bottomPadding: 16.0
                        leftPadding: 16.0
                        rightPadding: 16.0
                        bottomMargin: 12.0
                        background: Color.create("#FEFCBF")

                        Label {
                            text: "Incoming File Transfer!"
                            textStyle.base: SystemDefaults.TextStyles.TitleText
                            textStyle.color: Color.create("#B7791F")
                        }
                        Label {
                            text: "From: " + app.incomingPeerAlias + "\nFile: " + app.incomingFileName + " (" + app.incomingFileSize + ")"
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.color: Color.create("#2D3748")
                        }
                        Container {
                            layout: StackLayout {
                                orientation: LayoutOrientation.LeftToRight
                            }
                            topMargin: 12.0

                            Button {
                                text: "Accept"
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onClicked: {
                                    app.acceptIncoming(app.incomingRequestId);
                                }
                            }
                            Button {
                                text: "Decline"
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onClicked: {
                                    app.rejectIncoming(app.incomingRequestId);
                                }
                            }
                        }
                    }

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
                            onClicked: {
                                app.chooseFile();
                            }
                        }
                    }

                    Divider {}

                    // Nearby devices section
                    Header {
                        title: "Nearby Devices (" + app.peerCount + ")"
                    }

                    Label {
                        visible: app.peerCount === 0
                        text: app.isScanning ? "Scanning local subnet (192.168.x.x) for LocalSend peers..." : "No devices found. Tap Scan above or enter manual IP below."
                        textStyle.base: SystemDefaults.TextStyles.SubtitleText
                        textStyle.color: Color.create("#718096")
                    }

                    // Peer list rendering
                    Container {
                        id: peersContainer
                        objectName: "peersContainer"
                        topPadding: 8.0
                        bottomPadding: 8.0
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
                        onClicked: {
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

            attachedObjects: [
                ComponentDefinition {
                    id: candidateFileCardDef
                    Container {
                        id: candRoot
                        property string fileId: ""
                        property string fileName: ""
                        property string fileSize: ""
                        property bool isSelected: false

                        topPadding: 8.0
                        bottomPadding: 8.0
                        leftPadding: 12.0
                        rightPadding: 12.0
                        bottomMargin: 4.0
                        background: candRoot.isSelected ? Color.create("#CBD5E0") : Color.create("#EDF2F7")

                        Container {
                            layout: StackLayout {
                                orientation: LayoutOrientation.LeftToRight
                            }

                            Label {
                                text: (candRoot.isSelected ? "● " : "○ ") + candRoot.fileName + " (" + candRoot.fileSize + ")"
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 3.0
                                }
                                textStyle.base: SystemDefaults.TextStyles.BodyText
                                textStyle.color: candRoot.isSelected ? Color.create("#2B6CB0") : Color.create("#2D3748")
                                verticalAlignment: VerticalAlignment.Center
                            }

                            Button {
                                text: candRoot.isSelected ? "Selected" : "Select"
                                enabled: !candRoot.isSelected
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                verticalAlignment: VerticalAlignment.Center
                                onClicked: {
                                    app.selectIncomingFile(candRoot.fileId);
                                }
                            }
                        }
                    }
                }
            ]

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
                            text: "Selected: " + app.incomingFileName + " (" + app.incomingFileSize + ")"
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.color: Color.create("#2D3748")
                        }

                        // Candidate selection for multi-file offer
                        Label {
                            visible: app.incomingFiles.length > 1
                            text: "Choose file to receive (" + app.incomingFiles.length + " offered):"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#718096")
                            topMargin: 8.0
                        }

                        Container {
                            id: candidateFilesContainer
                            visible: app.incomingFiles.length > 1
                            topPadding: 4.0
                            bottomPadding: 4.0

                            property variant activeCandidateCards: []
                            property variant candidateList: app.incomingFiles
                            property string selectedCandidateId: app.selectedIncomingFileId

                            onCandidateListChanged: {
                                renderCandidates();
                            }
                            onSelectedCandidateIdChanged: {
                                renderCandidates();
                            }

                            function renderCandidates() {
                                for (var i = 0; i < activeCandidateCards.length; ++i) {
                                    if (activeCandidateCards[i]) {
                                        activeCandidateCards[i].destroy();
                                    }
                                }
                                activeCandidateCards = [];
                                candidateFilesContainer.removeAll();

                                var files = app.incomingFiles;
                                if (!files) {
                                    return;
                                }
                                var cards = [];
                                for (var j = 0; j < files.length; ++j) {
                                    var c = candidateFileCardDef.createObject();
                                    if (c) {
                                        var f = files[j];
                                        c.fileId = f.id;
                                        c.fileName = f.fileName;
                                        c.fileSize = f.formattedSize;
                                        c.isSelected = (f.id === app.selectedIncomingFileId);
                                        candidateFilesContainer.add(c);
                                        cards.push(c);
                                    }
                                }
                                activeCandidateCards = cards;
                            }

                            onCreationCompleted: {
                                renderCandidates();
                            }
                        }

                        Container {
                            layout: StackLayout {
                                orientation: LayoutOrientation.LeftToRight
                            }
                            topPadding: 12.0

                            Button {
                                text: "Accept"
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onClicked: {
                                    app.acceptIncoming(app.incomingRequestId, app.selectedIncomingFileId);
                                }
                            }
                            Button {
                                text: "Decline"
                                layoutProperties: StackLayoutProperties {
                                    spaceQuota: 1.0
                                }
                                onClicked: {
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
                        ProgressIndicator {
                            fromValue: 0.0
                            toValue: 100.0
                            value: app.transferProgress
                            state: ProgressIndicatorState.Progress
                            topMargin: 12.0
                            bottomMargin: 12.0
                        }

                        Button {
                            text: "Cancel Transfer"
                            enabled: app.transferActive
                            horizontalAlignment: HorizontalAlignment.Fill
                            onClicked: {
                                app.cancelTransfer();
                            }
                        }

                        Label {
                            visible: app.lastSavedFilePath.length > 0
                            text: "Saved file:\n" + app.lastSavedFilePath
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#2B6CB0")
                            topMargin: 8.0
                        }

                        Button {
                            visible: app.lastSavedFilePath.length > 0
                            text: "Open Received File"
                            horizontalAlignment: HorizontalAlignment.Fill
                            topMargin: 8.0
                            onClicked: {
                                app.openFile(app.lastSavedFilePath);
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
                    Button {
                        text: "Open Downloads Folder"
                        horizontalAlignment: HorizontalAlignment.Fill
                        topMargin: 8.0
                        onClicked: {
                            app.openReceiveDirectory();
                        }
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
                        onClicked: {
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
                        title: "Security & Certificate"
                    }
                    Label {
                        text: "Protocol: " + (app.isHttps ? "HTTPS" : "HTTP (Plaintext)")
                        textStyle.base: SystemDefaults.TextStyles.BodyText
                    }
                    Label {
                        text: "Certificate Fingerprint (SHA-256):"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#718096")
                    }
                    Label {
                        text: app.certificateFingerprint.length > 0 ? app.certificateFingerprint : "No certificate loaded"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#2B6CB0")
                    }

                    Divider {}

                    Header {
                        title: "Diagnostics & Logs"
                    }
                    Label {
                        text: "Diagnostic Log File:"
                        textStyle.base: SystemDefaults.TextStyles.BodyText
                    }
                    Label {
                        text: app.diagnosticLogPath
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#4A5568")
                    }
                    Button {
                        text: "Open Diagnostic Log"
                        horizontalAlignment: HorizontalAlignment.Fill
                        topMargin: 8.0
                        onClicked: {
                            app.openDiagnosticLog();
                        }
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
                        text: "Version 0.1.1 (Build 7)\nProtocol v2.2 Compliant\nNative Qt 4.8.5 & Cascades C++03"
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
