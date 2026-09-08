// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Camera",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "Camera",
            targets: ["Camera"]
        ),
    ],
    targets: [
        .target(
            name: "Camera",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("UIKit"),
            ]
        ),
    ]
)
