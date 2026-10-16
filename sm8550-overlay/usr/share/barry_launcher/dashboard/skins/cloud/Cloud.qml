// The Cloud skin's mark: a cloud with slanting rain under it, as cut into
// the keyboard case it is named after. The cloud's outline is Tabler Icons'
// "cloud" (MIT); drawn on a 24-unit grid and scaled.
import QtQuick
import QtQuick.Shapes

Shape {
    id: mark
    property color color: "white"
    property real lineWidth: 2

    preferredRendererType: Shape.CurveRenderer

    Shape {
        width: 24
        height: 24
        transform: Scale { xScale: mark.width / 24; yScale: mark.height / 24 }
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: mark.color
            strokeWidth: mark.lineWidth * 24 / Math.max(1, mark.width)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: "M6.657 15c-2.572 0 -4.657 -2.007 -4.657 -4.483c0 -2.475 2.085 -4.482 4.657 -4.482"
                    + "c.393 -1.762 1.794 -3.2 3.675 -3.773c1.88 -.572 3.956 -.193 5.444 1c1.488 1.19 2.162 3.007 1.77 4.769"
                    + "h.99c1.913 0 3.464 1.56 3.464 3.486c0 1.927 -1.551 3.487 -3.465 3.487h-11.878"
                    // rain: three rows of slanting dashes under the cloud's left
                    + "M3.5 17.5l1.4 -.8M6.5 17.5l1.4 -.8M9.5 17.5l1.4 -.8"
                    + "M2.5 19.7l1.4 -.8M5.5 19.7l1.4 -.8M8.5 19.7l1.4 -.8"
                    + "M1.5 21.9l1.4 -.8M4.5 21.9l1.4 -.8M7.5 21.9l1.4 -.8"
            }
        }
    }
}
