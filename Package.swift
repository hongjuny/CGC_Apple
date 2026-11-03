// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CGCApple",
    platforms: [.macOS(.v13)],
    targets: [
        .target(
            name: "A2Core",
            path: "Sources/A2Core",
            cSettings: [.unsafeFlags(["-O3"])]
        ),
        .executableTarget(
            name: "CGCApple",
            dependencies: ["A2Core"],
            path: "Sources/CGCApple"
        ),
    ]
)
