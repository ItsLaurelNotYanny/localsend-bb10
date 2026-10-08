import bb.cascades 1.2

Page {
    titleBar: TitleBar {
        title: "LocalSend"
    }
    Container {
        layout: StackLayout {}
        topPadding: 20.0
        leftPadding: 20.0
        rightPadding: 20.0
        bottomPadding: 20.0

        Label {
            text: "LocalSend for BlackBerry 10"
            textStyle.base: SystemDefaults.TextStyles.BigText
        }
        Label {
            id: statusLabel
            text: "Initializing protocol..."
            textStyle.base: SystemDefaults.TextStyles.BodyText
        }
    }
}
